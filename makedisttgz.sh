#!/bin/bash -v
set -e
cd `dirname $0`
./autogen.sh --prefix=/usr --with-large-heap=yes
make distdir
gitcommit=`git log -1 | grep ^commit`
gitcommit=${gitcommit:7:7}
gitbranch=`git branch | grep '[*]'`
gitbranch=${gitbranch:2}
mv mono-4.2.1 mono-$gitbranch
tar czf mono-$gitbranch-$gitcommit.tar.gz mono-$gitbranch
