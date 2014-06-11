mkdir -p `dirname $2`
while IFS='' read -r line; do eval "echo \"$line\""; done < $1 > $2
