 #MALLOB_PATH=/home/oguz/Desktop/hiwi_code/cbmc_mallob_monolithic/mallob
 
rm ./src/solvers/solvers.a ./src/cbmc/cbmc ./src/cbmc/libcbmc.a 
MALLOB=$MALLOB_PATH make -C src CXXFLAGS+=' -Wno-error ' -j16
