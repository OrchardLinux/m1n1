# Agent instructions

This is Orchard's m1n1: Asahi's m1n1 with Hoolock's A7-A11 work and Orchard's
changes on top. It is the source of the m1n1 Orchard builds. m1n1 work happens
here: branch from `main`, commit, and open the pull request in
OrchardLinux/m1n1. Never add m1n1 patch files to the orchard repository.

- Commits in upstream style: one topic each, `Assisted-by: LLM` when an LLM
  helped.
- Try a branch before it is merged: build it (`make EXTRA_CFLAGS=-DQUIET_BOOT`)
  and pass the result to orchard as `--loader FILE`.
- Orchard builds the commit named in its `boot/m1n1/upstream.lock`; once your
  pull request is merged, the orchard change that needs it moves that line
  (orchard's `boot/m1n1/README.md`).

LLM-assisted work is welcome here, but it stays in Orchard. Never upstream it:
no patches, pull requests, issues or comments to Asahi's or Hoolock's m1n1.
Asahi does not accept LLM-assisted contributions.
