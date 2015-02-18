import os
import sys
from threading import Thread
import time

total_time = 0
total_tasks = 0
end = 0

def RunCompilation(num):
  global total_tasks
  global total_time
  global end

  while True:
    start = time.clock_gettime(time.CLOCK_MONOTONIC)
    if os.system("./compile.sh /usr/bin/dist-clang/clang++ {} > /dev/null".format(num)) != 0:
      break
    total_tasks += 1
    total_time += time.clock_gettime(time.CLOCK_MONOTONIC) - start

    if len(sys.argv) > 2 and total_tasks >= int(sys.argv[2]):
      end = time.clock_gettime(time.CLOCK_MONOTONIC)
      break


if __name__ == "__main__":
  start = 0

  for i in range(0, int(sys.argv[1])):
    thread = Thread(target = RunCompilation, args = (i, ))
    thread.start()
  if len(sys.argv) > 2:
    start = time.clock_gettime(time.CLOCK_MONOTONIC)
  while True:
    time.sleep(10)
    if (total_tasks == 0):
      print("No complete tasks")
    else:
      print("Average time: " + str(total_time/total_tasks) + " per " + str(total_tasks) + " complete tasks")
    if start > 0 and end > 0:
      print(str(total_tasks) + " tasks took " + str(end - start))
      break
