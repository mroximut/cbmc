cd ../../mallob-ipasir-bridge
make -B
cd ../cbmc_mallob_filesystem/cbmc
rm src/solvers/solvers.a
make -C src LIBS="$PWD/../../mallob-ipasir-bridge/libipasirmallob.a" IPASIR=$PWD/../../mallob-ipasir-bridge/src -j16
