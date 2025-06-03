struct sysinfo {
  uint64 freemem;   // amount of free memory (bytes)
  uint64 nproc;     // number of process
  uint64 loadavg[3];//1 5 15 分钟平均负载（除以2^16表示小数）
};
