// MTX library in plain C. The SDK's PSMTX/PSVEC versions are paired-single
// assembly; these follow the documented SDK behaviour instead.
#include <math.h>
#include <string.h>

#include <revolution/MTX.h>

void PSMTXIdentity(Mtx m) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) {
    if (src != (const f32(*)[4])dst) {
        memcpy(dst, src, sizeof(Mtx));
    }
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx t;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
        }
    }
    memcpy(ab, t, sizeof(Mtx));
}

void PSMTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        PSMTXConcat(a, srcBase[i], dstBase[i]);
    }
}

void PSMTXTranspose(const Mtx src, Mtx xPose) {
    Mtx t;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            t[i][j] = src[j][i];
        }
        t[i][3] = 0.0f;
    }
    memcpy(xPose, t, sizeof(Mtx));
}

static f32 det33(const Mtx m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) -
           m[0][1] * (m[1][0] * m[2][2] - m[2][0] * m[1][2]) +
           m[0][2] * (m[1][0] * m[2][1] - m[2][0] * m[1][1]);
}

u32 PSMTXInverse(const Mtx src, Mtx inv) {
    f32 det = det33(src);
    if (det == 0.0f) {
        return 0;
    }
    f32 d = 1.0f / det;
    Mtx t;
    t[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * d;
    t[0][1] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * d;
    t[0][2] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * d;
    t[1][0] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * d;
    t[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * d;
    t[1][2] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * d;
    t[2][0] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * d;
    t[2][1] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * d;
    t[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * d;
    for (int i = 0; i < 3; i++) {
        t[i][3] = -(t[i][0] * src[0][3] + t[i][1] * src[1][3] + t[i][2] * src[2][3]);
    }
    memcpy(inv, t, sizeof(Mtx));
    return 1;
}

u32 PSMTXInvXpose(const Mtx src, Mtx invX) {
    Mtx inv;
    if (!PSMTXInverse(src, inv)) {
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            invX[i][j] = inv[j][i];
        }
        invX[i][3] = 0.0f;
    }
    return 1;
}

void PSMTXRotTrig(Mtx m, char axis, f32 s, f32 c) {
    PSMTXIdentity(m);
    switch (axis) {
    case 'x':
    case 'X':
        m[1][1] = c; m[1][2] = -s;
        m[2][1] = s; m[2][2] = c;
        break;
    case 'y':
    case 'Y':
        m[0][0] = c;  m[0][2] = s;
        m[2][0] = -s; m[2][2] = c;
        break;
    case 'z':
    case 'Z':
        m[0][0] = c; m[0][1] = -s;
        m[1][0] = s; m[1][1] = c;
        break;
    }
}

void PSMTXRotRad(Mtx m, char axis, f32 rad) {
    PSMTXRotTrig(m, axis, sinf(rad), cosf(rad));
}

void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    Vec n;
    PSVECNormalize(axis, &n);
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    f32 x = n.x, y = n.y, z = n.z;
    m[0][0] = t * x * x + c;     m[0][1] = t * x * y - s * z; m[0][2] = t * x * z + s * y; m[0][3] = 0.0f;
    m[1][0] = t * x * y + s * z; m[1][1] = t * y * y + c;     m[1][2] = t * y * z - s * x; m[1][3] = 0.0f;
    m[2][0] = t * x * z - s * y; m[2][1] = t * y * z + s * x; m[2][2] = t * z * z + c;     m[2][3] = 0.0f;
}

void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z) {
    PSMTXIdentity(m);
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
}

void PSMTXTransApply(const Mtx src, Mtx dst, f32 x, f32 y, f32 z) {
    PSMTXCopy(src, dst);
    dst[0][3] += x;
    dst[1][3] += y;
    dst[2][3] += z;
}

void PSMTXScale(Mtx m, f32 x, f32 y, f32 z) {
    PSMTXIdentity(m);
    m[0][0] = x;
    m[1][1] = y;
    m[2][2] = z;
}

void PSMTXScaleApply(const Mtx src, Mtx dst, f32 x, f32 y, f32 z) {
    const f32 s[3] = {x, y, z};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            dst[i][j] = src[i][j] * s[i];
        }
    }
}

void PSMTXMultVec(const Mtx m, const Vec* v, Vec* out) {
    Vec t;
    t.x = m[0][0] * v->x + m[0][1] * v->y + m[0][2] * v->z + m[0][3];
    t.y = m[1][0] * v->x + m[1][1] * v->y + m[1][2] * v->z + m[1][3];
    t.z = m[2][0] * v->x + m[2][1] * v->y + m[2][2] * v->z + m[2][3];
    *out = t;
}

void PSVECAdd(const Vec* a, const Vec* b, Vec* sum) {
    sum->x = a->x + b->x;
    sum->y = a->y + b->y;
    sum->z = a->z + b->z;
}

void PSVECScale(const Vec* in, Vec* out, f32 scale) {
    out->x = in->x * scale;
    out->y = in->y * scale;
    out->z = in->z * scale;
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

f32 PSVECMag(const Vec* v) {
    return sqrtf(PSVECDotProduct(v, v));
}

void PSVECNormalize(const Vec* in, Vec* out) {
    f32 inv = 1.0f / sqrtf(PSVECDotProduct(in, in));
    PSVECScale(in, out, inv);
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* prod) {
    Vec t;
    t.x = a->y * b->z - a->z * b->y;
    t.y = a->z * b->x - a->x * b->z;
    t.z = a->x * b->y - a->y * b->x;
    *prod = t;
}

f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

// C_ names used directly by the channel / nw4r.
void C_VECNormalize(const Vec* src, Vec* dst) { PSVECNormalize(src, dst); }
void C_VECCrossProduct(const Vec* a, const Vec* b, Vec* axb) { PSVECCrossProduct(a, b, axb); }

void C_VECHalfAngle(const Vec* a, const Vec* b, Vec* half) {
    Vec na = {-a->x, -a->y, -a->z};
    Vec nb = {-b->x, -b->y, -b->z};
    PSVECNormalize(&na, &na);
    PSVECNormalize(&nb, &nb);
    PSVECAdd(&na, &nb, half);
    if (PSVECDotProduct(half, half) > 0.0f) {
        PSVECNormalize(half, half);
    }
}

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp, const Point3d* target) {
    Vec look = {camPos->x - target->x, camPos->y - target->y, camPos->z - target->z};
    Vec right, up;
    PSVECNormalize(&look, &look);
    PSVECCrossProduct(camUp, &look, &right);
    PSVECNormalize(&right, &right);
    PSVECCrossProduct(&look, &right, &up);
    m[0][0] = right.x; m[0][1] = right.y; m[0][2] = right.z;
    m[0][3] = -(camPos->x * right.x + camPos->y * right.y + camPos->z * right.z);
    m[1][0] = up.x; m[1][1] = up.y; m[1][2] = up.z;
    m[1][3] = -(camPos->x * up.x + camPos->y * up.y + camPos->z * up.z);
    m[2][0] = look.x; m[2][1] = look.y; m[2][2] = look.z;
    m[2][3] = -(camPos->x * look.x + camPos->y * look.y + camPos->z * look.z);
}

void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS, f32 scaleT, f32 transS,
                       f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = (2.0f * n * tmp) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = ((r + l) * tmp) * scaleS - transS;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * n * tmp) * scaleT;
    m[1][2] = ((t + b) * tmp) * scaleT - transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = -1.0f; m[2][3] = 0.0f;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 angle = fovY * 0.5f * (3.14159265358979323846f / 180.0f);
    f32 cot = 1.0f / tanf(angle);
    m[0][0] = (cot / aspect) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = -1.0f; m[2][3] = 0.0f;
}

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = -(r + l) * tmp * scaleS + transS;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = -(t + b) * tmp * scaleT + transT;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 0.0f; m[2][3] = 1.0f;
}
