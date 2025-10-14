#!/bin/bash

emmake make PORTNAME=emscripten -C src/backend uninstall; emmake make PORTNAME=emscripten -C src/backend clean;
emmake make PORTNAME=emscripten -C pglite/ clean; emmake make PORTNAME=emscripten -C pglite/ uninstall;
emmake make PORTNAME=emscripten -C contrib/ clean; emmake make PORTNAME=emscripten -C contrib/ uninstall; 
emmake make PORTNAME=emscripten -C pglite clean; emmake make PORTNAME=emscripten -C pglite uninstall;
emmake make PORTNAME=emscripten clean; emmake make PORTNAME=emscripten uninstall;

echo "removing config.status"
rm config.status