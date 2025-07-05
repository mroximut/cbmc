 #MALLOB_PATH=/home/oguz/Desktop/hiwi_code/cbmc_mallob_monolithic/mallob
 
rm ./src/solvers/solvers.a ./src/cbmc/cbmc ./src/cbmc/libcbmc.a 
MALLOB=$MALLOB_PATH make -C src CXX=$(which mpicxx) CXXFLAGS+=' -Wno-error -DMALLOB_SUBPROC_DISPATCH_PATH=\"build_2ls/\"' -j16
