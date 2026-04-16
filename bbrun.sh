#!/usr/bin/env bash 

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

set -uo pipefail

topdir=$(pwd)

# running locally requires an ssh id for some reason 
if [[ ! -f "$HOME/.ssh/id_rsa.pem" ]]; then 
    ssh-keygen -m PEM -t rsa -b 4096 -f ~/.ssh/id_rsa.pem
fi 

echo "key=$HOME/.ssh/id_rsa.pem" > $topdir/scripts/deploy/config/key.conf

cat << EOF > ./config/kv_server.conf
iplist=(
    127.0.0.1
    127.0.0.1
    127.0.0.1
    127.0.0.1
    127.0.0.1
)
EOF

touch scripts/deploy/data/cert/admin.key.pub
touch ascripts/deploy/data/cert/admin.key.pri

cd $topdir/scripts/deploy 
./script/deploy_local.sh ./config/kv_server.conf
cd $topdir
