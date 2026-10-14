# Copy to tools/cluster.local.sh (git ignores it) and name your machines. tools/cluster.sh reads it.
# Every node needs: Linux on x86-64 with AVX2, gcc or clang, and ssh with key login from here
# (the names as in ~/.ssh/config).
CLUSTER_NODES="node1 node2"
# Shares of the work for search/verify/equiv; `tools/cluster.sh total` measures what they should be.
WEIGHTS="workstation=10 node1=10 node2=5"
# A process on the nodes whose core alpha leaves alone (optional), e.g. a long simulation.
PROTECT=""
