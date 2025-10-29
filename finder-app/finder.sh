#!/bin/bash

if [ $# -ne 2 ]; then
    echo "error: provide 2 arguments: filesdir and searchstr"
    exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d $filesdir ]; then
    echo "error: $filesdir does not exist"
    exit 1
fi

count=$(find "$filesdir" -type f | wc -l)
found=$(grep -r --binary-files=without-match -o "$searchstr" "$filesdir" 2>/dev/null | wc -l)

echo "The number of files are $count and the number of matching lines are $found"
