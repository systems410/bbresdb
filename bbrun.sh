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

set -eo pipefail

red="\x1B[31;1;1m"
blue="\x1B[34;1;1m"
yellow="\x1B[33;1;1m"
cyan="\x1B[36;1;1m"
green="\x1B[32;1;1m"
ecolor="\x1B[0m"

outputlog="deploylocal_log.txt"

info() { 
    echo -e "$blue[INFO]$ecolor $1"
}

debug=0
if [[ "$1" == "-d" ]]; then 
    debug=1
    debugid="${3:-5}"
fi 

show_loading() { 
    local pid=$!
    local message=$(info $1)
    local dots=(" " "." ".." "...")

    echo -ne "\x1B[?25l" 

    while kill -0 "$pid" &> /dev/null; do 
        for dot in "${dots[@]}"; do 
            echo -ne "\x1B[2K" 
            echo -n "$message$dot"
            echo -ne "\r"
            sleep 0.2
            kill -0 "$pid" &> /dev/null || break;  
        done
    done 
    echo -ne "\x1B[2K" 
    wait "$pid"  
    ret=$? 
    echo -ne "\x1B[?25h" 
    return $ret
}

success() { 
    echo -e "$green[SUCCESS]$ecolor\x1B[0m $1"
}

fatal() { 
    echo -e "$red[FATAL]$ecolor\x1B[0m $1"
    exit 1 
}

fatallog() { 
    echo -e "$red[FATAL]$ecolor\x1B[0m $1. Log written to $topdir/$outputlog"
    exit 1 
}


trap 'fatal "Something went wrong..."' ERR

topdir=$(pwd)

[[ -f "entrypoint.sh" ]] || fatal "Script must be run from the top level directory of bbresdb"

# running locally requires an ssh id for some reason 
if [[ ! -f "$HOME/.ssh/id_rsa.pem" ]]; then 
    info "SSH Key not found, press enter to each prompt"
    ssh-keygen -m PEM -t rsa -b 4096 -f ~/.ssh/id_rsa.pem || fatal "Could not create create ssh key"
fi 


echo "key=$HOME/.ssh/id_rsa.pem" > $topdir/scripts/deploy/config/key.conf

if (( debug == 0 )); then 
    cd $topdir/scripts/deploy 

    ./script/deploy_local.sh ./config/kv_server.conf > "$topdir/$outputlog" & 
    show_loading "Deploying" || fatallog "Deploy local failed"

    cd $topdir

    ps -A | grep kv_serv > /dev/null || fatallog "Deploy local script succeeded, but kv_service is not running. "

    success "KV Servers succesfully running"
else 
    cd $topdir/scripts/deploy 
    ./script/deploy_local.sh ./config/kv_server.conf "$@"
    cd $topdir
fi 
