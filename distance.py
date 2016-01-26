#!/usr/bin/python

import datetime
import time

# f = open("/sys/class/hcsr04/value",'r')

while True :
  f = open("/sys/class/hcsr04/value",'r')
  d=f.read()
  f.close()

#  if (float(d)==-1):
#	print "N.A."
#  else:	
  i = datetime.datetime.now()
  print "[%s] %.1f cm (%.1f in)" % (i.isoformat(), float(d)/58, float(d) / 148) 
#  print d

  time.sleep(0.5)
