# Host control · 8259 discovery and initialization

MSVC x64 `/W4 /WX /std:c11 /TC` compiled `lab/tests/pic8259_test.c`; the resulting program exited 0 and printed `PASS: PIC pair probe, normal-mode setup, independent phases, mask readback and refusal`.

The probe reads back `FBh` from the master mask. Normal cascaded initialization accepts master `11/30/04/01` and slave `11/38/02/01`. Each controller consumes its own words even when the sequences interleave. Refused commands leave both controllers unchanged; refused reads leave the caller’s byte untouched. This is a **host-model** result. No VM profiles, interrupt registrations, actual EOIs or guest Linux probe are qualified by this test.
