#!/bin/bash

set -x
set -e
set -o pipefail

# Pull the Hermes dependencies image
docker pull iowarp/iowarp-deps:latest
docker run -d \
--mount src=${PWD},target=/hermes,type=bind \
--name hermes_deps_c \
--network host \
--memory=8G \
--shm-size=8G \
-p 4000:4000 \
-p 4001:4001 \
iowarp/iowarp-deps \
tail -f /dev/null
