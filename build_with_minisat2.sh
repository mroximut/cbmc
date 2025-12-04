
rm src/solvers/solvers.a
make -C src minisat2-download
make -C src -j16
