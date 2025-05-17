* Put cbmc and mallob in the same directory.
```
├── cbmc/
└── mallob/
    └── build/
```
* Build mallob 
    * `cd mallob/build`
    * ` CC=$(which mpicc) CXX=$(which mpicxx) cmake -DCMAKE_BUILD_TYPE=RELEASE -DMALLOB_APP_SAT=1 -DMALLOB_USE_JEMALLOC=1 -DMALLOB_LOG_VERBOSITY=4 -DMALLOB_ASSERT=1 -DMALLOB_SUBPROC_DISPATCH_PATH=\"<path_to_malob>/build\" ..`
    * `rm build/*mallob*`
    * `make`
* Build cbmc
    * `cd cbmc`
    * ` rm ./src/solvers/solvers.a ./src/libcprover-cpp/libcprover-cpp.a `
    * ` MALLOB=<path_to_mallob> make -C src "CXXFLAGS+=-Wno-error" `
    * Run cbmc `./src/cbmc/cbmc --sat-solver mallob example2.c --verbosity 9`