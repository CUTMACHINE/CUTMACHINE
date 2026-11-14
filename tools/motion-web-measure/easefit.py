"""Best cubic-bezier fit (max error) for GSAP closed-form eases.

MOTION-WEB measurement, 2026-10-11: how far the motion format's
cubic-bezier vocabulary is from GSAP's polynomial eases.
"""
import math
import numpy as np
from scipy.optimize import minimize

def bez(x1, y1, x2, y2, xs):
    # Solve x(t)=xs by bisection, return y(t).
    t = np.linspace(0, 1, 4001)
    bx = 3*(1-t)**2*t*x1 + 3*(1-t)*t**2*x2 + t**3
    by = 3*(1-t)**2*t*y1 + 3*(1-t)*t**2*y2 + t**3
    return np.interp(xs, bx, by)

def pout(p): return lambda x: 1 - (1 - x)**p
def pin(p): return lambda x: x**p
def pinout(p): return lambda x: np.where(x < .5, (2*x)**p/2, 1 - (2*(1-x))**p/2)
E = {}
for k in range(1, 5):
    E[f"power{k}.out"] = pout(k+1); E[f"power{k}.in"] = pin(k+1); E[f"power{k}.inOut"] = pinout(k+1)
E["sine.inOut"] = lambda x: -(np.cos(np.pi*x)-1)/2
E["sine.out"] = lambda x: np.sin(x*np.pi/2)
E["expo.out"] = lambda x: np.where(x == 1, 1, 1 - 2**(-10*x))
E["circ.out"] = lambda x: np.sqrt(1 - (x-1)**2)
xs = np.linspace(0, 1, 401)
for name in ["power1.out","power2.out","power3.out","power4.out","power2.in","power2.inOut","power3.inOut","sine.inOut","sine.out","expo.out","circ.out"]:
    f = E[name](xs)
    best = None
    for x0 in ([.25,.1,.25,1],[.33,1,.68,1],[.5,0,.5,1],[.1,.9,.2,1],[.6,0,.9,.4]):
        r = minimize(lambda p: np.max(np.abs(bez(min(max(p[0],0),1),p[1],min(max(p[2],0),1),p[3],xs)-f)), x0, method="Nelder-Mead", options={"maxiter":4000,"xatol":1e-6,"fatol":1e-7})
        if best is None or r.fun < best.fun: best = r
    p = best.x
    print(f"{name:14s} erreur max {100*best.fun:5.2f} % de l'amplitude   cubic-bezier({p[0]:.3f}, {p[1]:.3f}, {p[2]:.3f}, {p[3]:.3f})")
