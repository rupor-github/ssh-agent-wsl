#!env bash

_dist=bin

[ -d ${_dist} ] && rm -rf ${_dist}

(
	cd linux

	[ -d build ] && rm -rf build
	mkdir build
	cd build

	cmake -DCMAKE_BUILD_TYPE=Release ..
	make install
)

(
	cd win32

	[ -d build ] && rm -rf build
	mkdir build
	cd build
	cmake -DCMAKE_BUILD_TYPE=Release ..
	make install
)

cd ${_dist}
7z a -r ../ssh-agent-wsl
