#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
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


if  ! ps -A | grep kv_serv > /dev/null; then 
    echo "KV Service not running" 
    exit 1
fi 

grep "replica" config_out/client.config > /dev/null || cp ../../service/tools/config/interface/service.config config_out/client.config 
bazel run //benchmark/protocols/pbft:kv_service_tools -- $PWD/config_out/client.config 

sleep 60

echo "benchmark done"
killall -9 kv_service

echo "getting results"

for (( idx=1; idx<6; idx++ )); do 
    cp ./resilientdb_app/${idx}/kv_service.log result_${idx}_log
done 

python3 performance/calculate_result.py `ls result_*_log` > results.log

rm -rf result_*_log
echo "save result to results.log"
cat results.log
