// SPDX-License-Identifier: MIT
//! Apple's Lightweight Volume Manager (LwVM) partition table, as iOS keeps
//! it on the flash of the 32-bit devices.
//!
//! The first 4 KiB of the disk hold the header: a signature, the media UUID
//! and size, a CRC32 of the header, up to eight partition descriptors and a
//! map of 1024 chunks. The disk is split into 1024 equal power-of-two
//! chunks; each map entry names the partition and the partition-relative
//! chunk stored there. A copy of the header ends the first chunk.
//!
//! Only partitions whose chunks are stored in order and back to back are
//! listed, each as one range of the disk. UUIDs are in network byte order.
//! Read-only.

use alloc::vec::Vec;
use fatfs::{Read, Seek, SeekFrom};
use uuid::Uuid;

const HEADER_SIZE: usize = 4096;
const BLOCK: u64 = 4096;
const CRC_OFFSET: usize = 0x2c;
const PARTS_OFFSET: usize = 0x200;
const PART_SIZE: usize = 0x80;
const MAX_PARTS: usize = 8;
const MAP_OFFSET: usize = 0x800;
const CHUNKS: usize = 1024;
const CHUNK_FREE: u16 = 0xf;

/// The CRC-protected variant; the older one without a CRC is not handled.
const SIGNATURE: [u8; 16] = [
    0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a, 0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b,
];

pub struct Partition {
    pub kind: Uuid,
    pub id: Uuid,
    /// First 4 KiB block on the disk
    pub first_block: u64,
    pub blocks: u64,
}

fn crc32(data: &[u8], skip: core::ops::Range<usize>) -> u32 {
    let mut crc = !0u32;
    for (i, byte) in data.iter().enumerate() {
        let byte = if skip.contains(&i) { 0 } else { *byte };
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb88320 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}

fn le16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes(b[at..at + 2].try_into().unwrap())
}

fn le32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

fn le64(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

/// The chunk is the smallest power of two that splits the disk in 1024.
fn chunk_size(media: u64) -> Option<u64> {
    if media < 2 * (HEADER_SIZE * CHUNKS) as u64 {
        return None;
    }
    Some((media.div_ceil(CHUNKS as u64)).next_power_of_two())
}

fn valid(header: &[u8]) -> bool {
    if header[..16] != SIGNATURE {
        return false;
    }
    let count = le32(header, 0x28) as usize;
    crc32(header, CRC_OFFSET..CRC_OFFSET + 4) == le32(header, CRC_OFFSET)
        && (1..=MAX_PARTS).contains(&count)
        && chunk_size(le64(header, 0x20)).is_some()
}

fn read_at<R: Read + Seek>(disk: &mut R, at: u64, out: &mut [u8]) -> Result<(), ()> {
    disk.seek(SeekFrom::Start(at)).map_err(|_| ())?;
    disk.read_exact(out).map_err(|_| ())
}

/// The contiguous partitions of an LwVM disk; `Ok(None)` if the disk has no
/// LwVM header, `Err` if it could not be read.
pub fn partitions<R: Read + Seek>(disk: &mut R) -> Result<Option<Vec<Partition>>, ()> {
    let mut header = alloc::vec![0u8; HEADER_SIZE];
    read_at(disk, 0, &mut header)?;
    if header[..16] != SIGNATURE {
        return Ok(None);
    }
    if !valid(&header) {
        // A damaged header: its copy ends the first chunk, where the size says
        let Some(chunk) = chunk_size(le64(&header, 0x20)) else {
            return Ok(None);
        };
        if read_at(disk, chunk - HEADER_SIZE as u64, &mut header).is_err() || !valid(&header) {
            return Ok(None);
        }
    }

    let media = le64(&header, 0x20);
    let chunk = chunk_size(media).ok_or(())?;
    let chunks = (media / chunk) as usize;
    let count = le32(&header, 0x28) as usize;

    // Each partition's first chunk, and how many chunks follow it in order
    let mut first = [0xffffu16; MAX_PARTS];
    let mut seen = [0u16; MAX_PARTS];
    for i in 0..CHUNKS {
        let entry = le16(&header, MAP_OFFSET + i * 2);
        let (owner, index) = ((entry >> 12) as usize, entry & 0xfff);
        if entry >> 12 == CHUNK_FREE {
            continue;
        }
        if i == 0 || i >= chunks || owner >= count {
            return Ok(None);
        }
        if index == 0 {
            first[owner] = i as u16;
        }
        if first[owner] != 0xffff && (i as u16 - first[owner]) == index && seen[owner] == index {
            seen[owner] += 1;
        } else {
            seen[owner] = 0xffff;
        }
    }

    let mut out = Vec::new();
    for i in 0..count {
        let at = PARTS_OFFSET + i * PART_SIZE;
        let (begin, end) = (le64(&header, at + 32), le64(&header, at + 40));
        if end <= begin || (begin | end) % BLOCK != 0 {
            continue;
        }
        let size = end - begin;
        if seen[i] == 0xffff
            || seen[i] == 0
            || (seen[i] as u64) * chunk < size
            || (seen[i] as u64 - 1) * chunk >= size
        {
            continue; // scattered over the disk: not one range
        }
        out.push(Partition {
            kind: Uuid::from_bytes(header[at..at + 16].try_into().unwrap()),
            id: Uuid::from_bytes(header[at + 16..at + 32].try_into().unwrap()),
            first_block: first[i] as u64 * (chunk / BLOCK),
            blocks: size / BLOCK,
        });
    }
    Ok(Some(out))
}
