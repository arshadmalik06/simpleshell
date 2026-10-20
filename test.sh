#!/bin/sh
# Feeds a set of commands into mysh (non-interactive mode, so no prompt is printed)
cd "$(dirname "$0")"
rm -f /tmp/mysh_out.txt
./mysh <<'CMDS'
echo hello world
echo "quoted   spaces   kept"
pwd
cd /tmp
pwd
ls /nonexistent_dir
nosuchcommand
echo one two three | wc -w
echo apple banana cherry | tr ' ' '\n' | sort -r | head -n 2
echo redirected > /tmp/mysh_out.txt
echo appended >> /tmp/mysh_out.txt
cat < /tmp/mysh_out.txt
sleep 1 &
echo after-background
help
exit 7
echo SHOULD-NOT-PRINT
CMDS
echo "exit status of mysh: $?"
