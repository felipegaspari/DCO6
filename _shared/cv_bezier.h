#ifndef __CV_BEZIER_H__
#define __CV_BEZIER_H__

// Port of Mainboard auxiliary.h Bézier helpers for AS2164 VCA linearize table.

struct Point {
  float x, y;
};

inline Point bezierCubic(const Point& A, const Point& P1, const Point& P2, const Point& B, float t) {
  float one_minus_t = 1.0f - t;
  float one_minus_t_squared = one_minus_t * one_minus_t;
  float t_squared = t * t;
  float x = one_minus_t_squared * one_minus_t * A.x + 3 * one_minus_t_squared * t * P1.x +
            3 * one_minus_t * t_squared * P2.x + t_squared * t * B.x;
  float y = one_minus_t_squared * one_minus_t * A.y + 3 * one_minus_t_squared * t * P1.y +
            3 * one_minus_t * t_squared * P2.y + t_squared * t * B.y;
  return { x, y };
}

inline float findYForX(const Point& A, const Point& P1, const Point& P2, const Point& B, float xTarget,
                       float tol = 1e-5f) {
  float tLow = 0.0f;
  float tHigh = 1.0f;
  float tMid;

  while ((tHigh - tLow) > tol) {
    tMid = (tLow + tHigh) / 2.0f;
    Point midPoint = bezierCubic(A, P1, P2, B, tMid);
    if (midPoint.x < xTarget) {
      tLow = tMid;
    } else {
      tHigh = tMid;
    }
  }

  Point resultPoint = bezierCubic(A, P1, P2, B, tMid);
  return resultPoint.y;
}

inline void generateBezierArray(Point A, Point B, Point P1, Point P2, uint16_t arraySize,
                                uint16_t (&array)[4096]) {
  for (int x = 0; x < arraySize; ++x) {
    float yResult = findYForX(A, P1, P2, B, static_cast<float>(x));
    array[x] = (uint16_t)yResult;
  }
}

#endif
