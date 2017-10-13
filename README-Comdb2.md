## Building sysbench with Comdb2 support

```
mkdir <sysbench-install-dir>
./autogen.sh
./configure --with-comdb2 --prefix=<sysbench-install-dir>
make
make install

export PATH=$PATH:<sysbench-install-dir>/bin:<sysbench-install-dir>/share/sysbench
export LD_LIBRARY_PATH=/opt/bb/lib/

sysbench --db-driver=comdb2 --comdb2-db=test --comdb2-host=local --auto_inc=off [test] [stage]
```
