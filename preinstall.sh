#!/bin/bash

if [[ -d 3rdparty/readline ]]; then
    npm run build
    exit 0
fi

npm install nan
mkdir -p 3rdparty
cd 3rdparty
git clone https://git.savannah.gnu.org/git/readline.git
cd readline
git checkout readline-7.0
cd ..
INSTALLPATH=${PWD}/readline-install
mkdir readline-build
cd readline-build
CFLAGS=-fPIC LDFLAGS=-fPIC ../readline/configure --prefix=$INSTALLPATH --disable-shared --enable-multibyte
make
make install
cd ../..
npm run build
