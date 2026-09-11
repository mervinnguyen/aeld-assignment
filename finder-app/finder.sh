#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Error: two arguments required - filesdir searchstr"
    exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d "$filesdir" ]
then
    echo "Error: $filesdir is not a directory"
    exit 1
fi

numfiles=$(find "$filesdir" -type f | wc -l)
numlines=$(grep -r "$searchstr" "$filesdir" 2>/dev/null | wc -l)

echo "The number of files are $numfiles and the number of matching lines are $numlines"
