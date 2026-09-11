#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Error: two arguments required - writefile writestr"
    exit 1
fi

writefile=$1
writestr=$2

writedir=$(dirname "$writefile")

mkdir -p "$writedir"
if [ $? -ne 0 ]
then
    echo "Error: could not create directory $writedir"
    exit 1
fi

echo "$writestr" > "$writefile"
if [ $? -ne 0 ]
then
    echo "Error: could not create file $writefile"
    exit 1
fi
