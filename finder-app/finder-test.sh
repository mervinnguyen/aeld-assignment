#!/bin/sh
# Tester script for assignment 1 and assignment 2
# Author: Siddhant Jajoo

set -e
set -u

NUMFILES=10
WRITESTR=AELD_IS_FUN
WRITEDIR=/tmp/aeld-data
RESULTFILE=/tmp/assignment4-result.txt

# On the target, writer and finder.sh are installed in /usr/bin and config
# lives in /etc/finder-app/conf. When run from the source tree instead, fall
# back to the executables and conf directory next to this script.
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
if ! command -v writer >/dev/null 2>&1 || ! command -v finder.sh >/dev/null 2>&1
then
	PATH="${SCRIPTDIR}:${PATH}"
fi
if [ -d /etc/finder-app/conf ]
then
	CONFDIR=/etc/finder-app/conf
else
	CONFDIR="${SCRIPTDIR}/conf"
fi

username=$(cat "${CONFDIR}/username.txt")

if [ $# -lt 3 ]
then
	echo "Using default value ${WRITESTR} for string to write"
	if [ $# -lt 1 ]
	then
		echo "Using default value ${NUMFILES} for number of files to write"
	else
		NUMFILES=$1
	fi
else
	NUMFILES=$1
	WRITESTR=$2
	WRITEDIR=/tmp/aeld-data/$3
fi

MATCHSTR="The number of files are ${NUMFILES} and the number of matching lines are ${NUMFILES}"

echo "Writing ${NUMFILES} files containing string ${WRITESTR} to ${WRITEDIR}"

rm -rf "${WRITEDIR}"

# create $WRITEDIR if not assignment1
assignment=$(cat "${CONFDIR}/assignment.txt")

if [ $assignment != 'assignment1' ]
then
	mkdir -p "$WRITEDIR"

	#The WRITEDIR is in quotes because if the directory path consists of spaces, then variable substitution will consider it as multiple argument.
	#The quotes signify that the entire string in WRITEDIR is a single string.
	#This issue can also be resolved by using double square brackets i.e [[ ]] instead of using quotes.
	if [ -d "$WRITEDIR" ]
	then
		echo "$WRITEDIR created"
	else
		exit 1
	fi
fi

for i in $( seq 1 $NUMFILES)
do
	writer "$WRITEDIR/${username}$i.txt" "$WRITESTR"
done

OUTPUTSTRING=$(finder.sh "$WRITEDIR" "$WRITESTR")
echo "${OUTPUTSTRING}" > "${RESULTFILE}"

# remove temporary directories
rm -rf /tmp/aeld-data

set +e
echo ${OUTPUTSTRING} | grep "${MATCHSTR}"
if [ $? -eq 0 ]; then
	echo "success"
	exit 0
else
	echo "failed: expected  ${MATCHSTR} in ${OUTPUTSTRING} but instead found"
	exit 1
fi
