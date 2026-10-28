# Traceability — eCall TCU baseline v1

Maps each requirement to its implementation and the tests that verify it.
All data is synthetic.

| Req ID   | Implementation                                               | Tests (Unity / ctest)                                           |
|----------|--------------------------------------------------------------|-----------------------------------------------------------------|
| ECALL-01 | `ap/libs/can_if/can_if.c` (`can_if_decode_frame`, `can_if_decode_sos`, `can_if_poll_can`, `can_if_poll_sos`) | `test_can`: `test_ECALL_CAN_01..07` |
| ECALL-02 | `ap/libs/gnss_if/gnss_if.c` (`gnss_if_parse_sentence`, `gnss_if_poll`, `gnss_if_get_latest`) | `test_gnss`: `test_ECALL_GNSS_01..06` |
| ECALL-03 | `ap/libs/ecall_core/ecall_core.c` (state machine, duplicate-trigger guard) | `test_core`: `test_ECALL_CORE_01..05` |
| ECALL-04 | `ap/libs/msd/msd.c` (`msd_encode`), layout in `docs/msd_layout.md` | `test_msd`: `test_ECALL_MSD_01, 02, 07` |
| ECALL-05 | `ap/libs/msd/msd.c` (`msd_fill_position`, 2 s freshness rule) | `test_msd`: `test_ECALL_MSD_03, 04, 05` |
| ECALL-06 | `ap/libs/psap_if/psap_if.c` (`psap_if_send_with_retry`, `psap_if_send_msd`) | `test_psap`: `test_ECALL_PSAP_01..05` |
| ECALL-07 | `ap/services/ecalld/ecalld.c` (epoll loop, syslog), `ecalld_config.c`, `ecalld.service` | manual / integration (no unit test in v1) |
| ECALL-08 | `ap/libs/msd/msd.c` (`MSD_COORD_NOT_AVAILABLE`, never `0,0`) | `test_msd`: `test_ECALL_MSD_04, 05, 08` |

## Tooling

| Artifact                 | Purpose                                               |
|--------------------------|-------------------------------------------------------|
| `tools/gnss_sim.py`      | Synthetic NMEA stream over UDP 10110; `--cut-at-impact` simulates GNSS loss. |
| `tools/crash_inject.sh`  | Inject AUTOMATIC (CAN 0x0A0#01..) or MANUAL (UDP SOS) triggers. |

## Notes

- v1 has **no** GNSS-loss fallback. When GNSS is cut (e.g. after impact via
  `gnss_sim.py --cut-at-impact`), the MSD reports `NOT_AVAILABLE` rather than a
  stale or estimated position. This gap is the motivation for future work.
