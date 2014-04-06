mkdir -p `dirname $2`
while read line; do eval echo \"$line\"; done < $1 > $2
