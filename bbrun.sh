#!/usr/bin/env bash 

set -uo pipefail

red="\x1B[31;1;1m"
blue="\x1B[34;1;1m"
yellow="\x1B[33;1;1m"
cyan="\x1B[36;1;1m"
green="\x1B[32;1;1m"
ecolor="\x1B[0m"

fatal() { 
    echo -e "$red[FATAL]$ecolor\x1B[0m $1"
    exit 1 
}

info() { 
    echo -e "$blue[INFO]$ecolor $1"
}

topdir=$(pwd)

(( $EUID != 0 )) && fatal "Run this script as root" 

which ssh > /dev/null || apt install ssh 

if ! which bazel; then 
    bazelpath=$(which bazel-6.0.0) || fatal "Bazel not found" 
    info "Creating bazel symlink to /usr/bin/bazel from $bazelpath" 
    ln "$bazelpath" "/usr/bin/bazel" 
fi 

if [[ ! -f "$HOME/.ssh/id_rsa.pem" ]]; then 
    ssh-keygen -m PEM -t rsa -b 4096 -f ~/.ssh/id_rsa.pem
    echo "key=$HOME/.ssh/id_rsa.pem" > $topdir/scripts/deploy/config/key.conf
    info "SSH key generated"
fi 

cd $topdir/scripts/deploy 

islocal=$(grep -w "127.0.0.1" ./config/kv_server.conf)

if [[ -z islocal ]]; then 
    info "Writing server config" 
    cat << EOF > ./config/kv_server.conf
iplist=(
    127.0.0.1
    127.0.0.1
    127.0.0.1
    127.0.0.1
    127.0.0.1
)
EOF
fi 

info "Running deployment script" 
./script/deploy_local.sh ./config/kv_server.conf
cd $topdir