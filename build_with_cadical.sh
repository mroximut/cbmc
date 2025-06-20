git clone --branch rel-2.1.0 https://github.com/arminbiere/cadical.git
cd cadical
./configure
make cadical -j16
cd ..
make -C src LIBS="$PWD/cadical/build/libcadical.a" IPASIR=$PWD/cadical/src -j16