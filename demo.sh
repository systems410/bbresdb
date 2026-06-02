
set -eo pipefail

red="\x1B[31;1;1m"
blue="\x1B[34;1;1m"
yellow="\x1B[33;1;1m"
cyan="\x1B[36;1;1m"
green="\x1B[32;1;1m"
ecolor="\x1B[0m"


echo -e "${blue}Sharded Replicated 2PC/Paxos Demo${ecolor}" 
echo

echo -e "${green}Deploying Servers${ecolor}" 
./bbrun.sh 2&> /dev/null 
echo 

echo -e "${yellow}Requesting key value store (Set key1: valu1, Get key1)${ecolor}" 
echo 
echo -e "${cyan}Client result${ecolor}"
echo 
./keytest.sh
echo 

cd scripts/deploy/resilientdb_app
echo -e "${cyan}Node 1 Logs (2PC Coordinator, Paxos Leader)${ecolor}"
echo 
grep -oP '\[(2PC|PAXOS)\].*' 1/kv_service.log

cd ../../../
