#!/usr/bin/env bash

#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
#
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

set -e

# load environment parameters
. ./script/env.sh

# load ip list
. ./script/load_config.sh $1

debug=0
if [[ "$2" == "-d" ]]; then 
    debug=1
    which gdb > /dev/null || apt install gdb
    debugid="${3:-5}"
fi 


script_path=${BAZEL_WORKSPACE_PATH}/scripts

if [[ -z $server ]];
then
server=//service/kv:kv_service
fi

# obtain the src path
main_folder=resilientdb_app
server_path=`echo "$server" | sed 's/:/\//g'`
server_path=${server_path:1}
server_name=`echo "$server" | awk -F':' '{print $NF}'`
server_bin=${server_name}
grafna_port=8090

bin_path=${BAZEL_WORKSPACE_PATH}/bazel-bin/${server_path}
output_path=${script_path}/deploy/config_out
output_key_path=${output_path}/cert
output_cert_path=${output_key_path}
export home_path=${script_path}/deploy

admin_key_path=${script_path}/deploy/data/cert

rm -rf ${output_path}
mkdir -p ${output_path}

deploy_iplist=${iplist[@]}

echo "server src path:"${server_path}
echo "server bazel bin path:"${bin_path}
echo "server name:"${server_bin}
echo "admin config path:"${admin_key_path}
echo "output path:"${output_path}
echo "deploy to :"${deploy_iplist[@]}

# generate keys and certificates.

cd ${script_path}
echo "where am i:"$PWD

deploy/script/generate_admin_key.sh ${BAZEL_WORKSPACE_PATH} ${admin_key_path} 
deploy/script/generate_key.sh ${BAZEL_WORKSPACE_PATH} ${output_key_path} ${#iplist[@]}
deploy/script/generate_config.sh ${BAZEL_WORKSPACE_PATH} ${output_key_path} ${output_cert_path} ${output_path} ${admin_key_path} ${deploy_iplist[@]}

# build kv server
bazel build ${server}

if [ $? != 0 ]
then
	echo "Complile ${server} failed"
	exit 1
fi


# commands functions
function run_cmd(){
  echo "run cmd:"$1
  local c=1
  local i=1
  for ip in ${deploy_iplist[@]};
  do
    cd "${home_path}/${main_folder}/$i"; 
    `$1`
    ((c++))
    ((i++))
  done

  while [ $c -gt 0 ]; do
        wait $pids
        c=`expr $c - 1`
  done
}

function run_one_cmd(){
  echo "run one:"$1
  $1
}

killall -9 ${server_bin}

if [ $performance ];
then
 rm -rf ${home_path}/${main_folder}
fi

for (( i=1; i<6; i++ )); do
  mkdir -p "${home_path}/${main_folder}/$i"
done

# upload config files and binary
echo "upload configs"
idx=1
count=0
for ip in ${deploy_iplist[@]};
do
  echo "cp -r ${bin_path} ${output_path}/server.config ${output_path}/cert ${home_path}/${main_folder}/$idx" 
  cp -r ${bin_path} ${output_path}/server.config ${output_path}/cert ${home_path}/${main_folder}/$idx &
  ((count++))
  ((idx++))
done

while [ $count -gt 0 ]; do
  wait $pids
  count=`expr $count - 1`
done

echo "start to run" $PWD

if ((debug == 0)); then 
  # Start server
  idx=1
  count=0
  for ip in ${deploy_iplist[@]};
  do
    private_key="cert/node_"${idx}".key.pri"
    cert="cert/cert_"${idx}".cert"
    cd ${home_path}/${main_folder}/$idx; 
    nohup ./${server_bin} server.config ${private_key} ${cert} ${grafna_port} > ${server_bin}.log 2>&1 &
    ((count++))
    ((idx++))
    ((grafna_port++))
  done
  # Check ready logs
  idx=1
  for ip in ${deploy_iplist[@]};
  do
    resp=""
    while [ "$resp" = "" ]
    do
        cd ${home_path}/${main_folder}/$idx;
        resp=`grep "receive public size:${#iplist[@]}" ${server_bin}.log` 
        if [ "$resp" = "" ]; then
        sleep 1
        fi
    done
    ((idx++))
    done

    echo "Servers are running....."
else 
  # DEBUG MODE 
  idx=1
  count=0
  debugport=0
  for ip in ${deploy_iplist[@]};
  do
    if (( debugid != idx )); then 
      private_key="cert/node_"${idx}".key.pri"
      cert="cert/cert_"${idx}".cert"
      cd ${home_path}/${main_folder}/$idx; nohup ./${server_bin} server.config ${private_key} ${cert} ${grafna_port} > ${server_bin}.log 2>&1 &
    else
      debugport="$grafna_port"
    fi 
    ((count++))
    ((idx++))
    ((grafna_port++))
  done
  private_key="cert/node_"${debugid}".key.pri"
  cert="cert/cert_"${debugid}".cert"
  cd ${home_path}/${main_folder}/$debugid
  gdb -ex "set directories ../../../../" -ex "set logging file deploylocal.txt" --args ./${server_bin} server.config ${private_key} ${cert}  
#   gdb -eval-command="set directories ../../../../" -eval-command="set logging file deploylocal.txt" --args ./${server_bin} server.config ${private_key} ${cert} ${debugport} 
  
fi 




