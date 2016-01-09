#!/bin/bash -v
#
#  Create distribution tar/gz file from fresh clone
#
set -e
cd `dirname $0`
srcdir=`pwd`
rm -rf insdir
mkdir insdir
cd insdir
insdir=`pwd`
cd $srcdir
./autogen.sh --prefix=$insdir --with-large-heap=yes
make
make install
export PATH=$insdir/bin:/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
make distdir
version=`grep '^VERSION' Makefile`
version=${version#*= }
gitcommit=`git log -1 | grep ^commit`
gitcommit=${gitcommit:7:7}
gitbranch=`git branch | grep '[*]'`
gitbranch=${gitbranch:2}
mv mono-$version mono-$gitbranch
tar czf mono-$gitbranch-$gitcommit.tar.gz mono-$gitbranch
