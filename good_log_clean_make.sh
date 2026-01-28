#!/bin/bash
make clean
make 2>&1 | tee build.log
