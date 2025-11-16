#!/bin/sh

if [ $# -ne 2 ]; then
    echo "error: provide 2 arguments: writefile and writestr"
    exit 1
fi

writefile=$1
writestr=$2

mkdir -p "$(dirname "$writefile")"
echo "$writestr" > "$writefile"
