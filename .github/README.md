## Yamagi Quake II client for MiyooCFW

The Yamagi Quake II Client is an enhanced version of id Software's Quake II with focus on offline and coop gameplay. This code is build upon Icculus Quake II, which itself is based on Quake II 3.21 (quoted from oe README file).

### Build steps:

- native (Linux)  
`make -j$(nproc)`

- cross-compile (MiyooCFW)  
`make -j$(nproc) -f Makefile.miyoo`

### Requirements

- native
`./baseq2` with necessary pak assets in $CWD of target binary (you can change path with `-datadir` arg cmd)

- MiyooCFW
place `baseq2` assets directory in `/roms/QUAKE_II` dir