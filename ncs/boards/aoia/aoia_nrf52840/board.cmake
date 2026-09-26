# Use UF2 by default; an SWD probe is optional. Never mass-erase this board.
board_runner_args(jlink "--device=nRF52840_xxAA" "--speed=4000")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
