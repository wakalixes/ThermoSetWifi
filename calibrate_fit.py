import numpy as np
import matplotlib.pyplot as plt
from scipy.optimize import curve_fit

def func1(x, a, b):
    return a * x + b

def func3(x, a, b, c, d):
    return a * x**3 + b * x**2 + c * x + d

xdata = [-8,-5,0,5,10,15,20,25,30,35,40]
ydata = [7080,6840,6400,5970,5560,5090,4620,4120,3680,3240,2860]

xfit = np.linspace(-20, 40, 100)
y1 = func1(xfit, -100, 6300)
y3 = func3(xfit, 1, -1, -100, 6300)

popt1, pcov1 = curve_fit(func1, xdata, ydata)
print(popt1)
popt3, pcov3 = curve_fit(func3, xdata, ydata)
print(popt3)

plt.plot(xfit, func1(xfit, *popt1), 'g-', label='fit: a=%5.3f, b=%5.3f' % tuple(popt1))
plt.plot(xfit, func3(xfit, *popt3), 'r-', label='fit: a=%5.3f, b=%5.3f, c=%5.3f, d=%5.3f' % tuple(popt3))
plt.plot(xdata, ydata, 'b.', label='data')

plt.xlabel('Temperatur [°C]')
plt.ylabel('DAC value')
plt.legend()
plt.show()