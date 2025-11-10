rm ./src/solvers/solvers.a ./src/cbmc/cbmc ./src/cbmc/libcbmc.a ./src/libcprover-cpp/libcprover-cpp.a
MALLOB=$MALLOB_PATH make -C src CXXFLAGS+=' -Wno-error ' -j16
