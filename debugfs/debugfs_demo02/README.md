
```bash
make && sudo insmod nzfs_readahead.ko

# 读取
cat /sys/kernel/debug/nzfs_readahead/backend_bandwidth
# backend_bandwidth: 0 MB/s

# 写入
echo 2048 | sudo tee /sys/kernel/debug/nzfs_readahead/backend_bandwidth

# 再次读取
cat /sys/kernel/debug/nzfs_readahead/backend_bandwidth
# backend_bandwidth: 2048 MB/s

sudo rmmod nzfs_readahead
```