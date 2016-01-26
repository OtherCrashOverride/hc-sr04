#Linux Driver for ODROID-C1/C1+ and HC-SR04 ultrasonic ranger

Default trigger GPIO = 104 (J2 - Pin16)
Default echo GPIO = 102 (J2 - Pin18)

Use module parameters to override defaults:
```
sudo insmod hcsr04.ko [trigger_gpio=xxx] [echo_gpio=yyy]
```

Note:
HC-SR04 modules are not very accurate at long distances.  Erratic readings
when measuring large distances are due to the sensor hardware.

See the following for more details:
http://uglyduck.ath.cx/ep/archive/2014/01/Making_a_better_HC_SR04_Echo_Locator.html

