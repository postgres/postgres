#!/bin/bash

emmake make -C src/backend uninstall; emmake make -C src/backend clean;
emmake make -C pglite/ clean; emmake make -C pglite/ uninstall;
emmake make -C contrib/ clean; emmake make -C contrib/ uninstall; emmake make -C pglite clean; emmake make -C pglite uninstall;
emmake make clean; emmake make uninstal