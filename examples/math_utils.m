-- math_utils.m  --  Example Nova macro/header file
-- Demonstrates preprocessor features

#ifndef MATH_UTILS_M
#define MATH_UTILS_M

-- Mathematical constants
#define PI          3.14159265358979323846
#define TAU         (PI * 2)
#define E           2.71828182845904523536
#define SQRT2       1.41421356237309504880

-- Conversion macros
#define DEG2RAD(d)  ((d) * PI / 180.0)
#define RAD2DEG(r)  ((r) * 180.0 / PI)

-- Clamping
#define CLAMP(x, lo, hi)  (((x) < (lo)) and (lo) or (((x) > (hi)) and (hi) or (x)))
#define MIN(a, b)          ((a) < (b) and (a) or (b))
#define MAX(a, b)          ((a) > (b) and (a) or (b))

-- Approximate equality for floating point
#define EPSILON 1e-10
#define APPROX_EQ(a, b)  (math.abs((a) - (b)) < EPSILON)

#endif
