#pragma once
// Render-only headwear math. Native matrices use row vectors (18C70).
#include <cmath>
#include <cstring>
#include "camera_motion.h"
#include "input_policy.h"
namespace fp::wear {
inline constexpr unsigned maxFaces=32768, maxVertices=32768, maxBones=256, maxGroups=64;
enum class Face : unsigned char { native, head, gear, blocked };
inline void identity(float* m) {
    std::memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1.f;
}
inline void product(const float* a,const float* b,float* out) {
    float t[16];
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
        t[r*4+c]=(a[r*4]*b[c]+a[r*4+1]*b[4+c])+(a[r*4+2]*b[8+c]+a[r*4+3]*b[12+c]);
    std::memcpy(out,t,64);
}
inline bool inverse(const float* m,float* out) {
    double a[4][8]{};
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) {
        if(!std::isfinite(m[r*4+c])) return false;
        a[r][c]=m[r*4+c]; a[r][c+4]=r==c?1.:0.;
    }
    for(unsigned c=0;c<4;++c) {
        unsigned p=c;
        for(unsigned r=c+1;r<4;++r) if(std::fabs(a[r][c])>std::fabs(a[p][c])) p=r;
        if(std::fabs(a[p][c])<1.e-10) return false;
        if(p!=c) for(unsigned k=0;k<8;++k) { double t=a[p][k];a[p][k]=a[c][k];a[c][k]=t; }
        const double d=a[c][c]; for(unsigned k=0;k<8;++k) a[c][k]/=d;
        for(unsigned r=0;r<4;++r) if(r!=c) {
            const double f=a[r][c];for(unsigned k=0;k<8;++k) a[r][k]-=f*a[c][k];
        }
    }
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) {
        const float v=float(a[r][c+4]); if(!std::isfinite(v)) return false;out[r*4+c]=v;
    }
    return true;
}
inline bool affine(const float* pose,float* out) {
    identity(out);
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c) {
        const float v=pose[r*3+c]; if(!std::isfinite(v)) return false;out[r*4+c]=v;
    }
    return true;
}
inline bool point(const float* m,const float* p,float* out,bool direction=false) {
    float t[3];
    for(unsigned c=0;c<3;++c) {
        t[c]=(p[0]*m[c]+p[1]*m[4+c])+p[2]*m[8+c]+(direction?0.f:m[12+c]);
        if(!std::isfinite(t[c])) return false;
    }
    std::memcpy(out,t,12);return true;
}
// The view matrix belongs to THIS native temporal sample. Never attach to the
// last nav publication, which can be a different supersampling phase.
inline bool mount(const float* view,float setback,float* out) {
    float camera[16];if(!inverse(view,camera)) return false;
    identity(out);
    for(unsigned c=0;c<3;++c) {
        out[c]=-camera[c]; out[4+c]=camera[4+c]; out[8+c]=-camera[8+c];
        out[12+c]=camera[12+c]-out[4+c]*eyeAlongAxis2-
                  out[8+c]*(eyeAlongAxis3-float(clampEyeSetback(int(setback))));
    }
    return true;
}
// Two-sided, finite segment/triangle test. Tiny/degenerate faces cannot be
// blockers. All inputs are copied render geometry, never collision data.
inline bool triangleHit(const float* a,const float* b,const float* c,const float* origin,
                        const float* direction,float length) {
    double e1[3],e2[3],s[3],p[3],q[3];
    for(unsigned k=0;k<3;++k) {e1[k]=double(b[k])-a[k];e2[k]=double(c[k])-a[k];s[k]=double(origin[k])-a[k];}
    p[0]=direction[1]*e2[2]-direction[2]*e2[1];
    p[1]=direction[2]*e2[0]-direction[0]*e2[2];
    p[2]=direction[0]*e2[1]-direction[1]*e2[0];
    const double d=e1[0]*p[0]+e1[1]*p[1]+e1[2]*p[2];
    if(!std::isfinite(d)||std::fabs(d)<1.e-10) return false;
    const double u=(s[0]*p[0]+s[1]*p[1]+s[2]*p[2])/d;
    if(u<0.||u>1.) return false;
    q[0]=s[1]*e1[2]-s[2]*e1[1];q[1]=s[2]*e1[0]-s[0]*e1[2];q[2]=s[0]*e1[1]-s[1]*e1[0];
    const double v=(direction[0]*q[0]+direction[1]*q[1]+direction[2]*q[2])/d;
    const double t=(e2[0]*q[0]+e2[1]*q[1]+e2[2]*q[2])/d;
    return std::isfinite(t)&&v>=0.&&u+v<=1.&&t>=0.&&t<=length;
}
inline bool blocks(const float* a,const float* b,const float* c,float setback) {
    const float s=std::sin(helmetConeDegrees*3.14159265358979323846f/180.f);
    const float z=std::cos(helmetConeDegrees*3.14159265358979323846f/180.f);
    const float dirs[5][3]={{0,0,1},{s,0,z},{-s,0,z},{0,s,z},{0,-s,z}};
    const float eye[3]={0,eyeAlongAxis2,eyeAlongAxis3-float(clampEyeSetback(int(setback)))};
    for(const auto& d:dirs) if(triangleHit(a,b,c,eye,d,helmetProbeLength)) return true;
    return false;
}
struct Weights { int bone[4];float weight[4]; };
inline bool headVertex(const Weights& w,const bool* headBones,unsigned count) {
    float total=0.f;bool any=false;
    for(unsigned k=0;k<4;++k) {
        if(!std::isfinite(w.weight[k])||w.weight[k]<0.f) return false;
        if(w.bone[k]<1) { if(w.weight[k]!=0.f) return false;continue; }
        if(unsigned(w.bone[k])>count) return false;
        if(w.weight[k]>0.f) { if(!headBones[w.bone[k]-1]) return false;any=true;total+=w.weight[k]; }
    }
    return any&&std::fabs(total-1.f)<=0.001f;
}
} // namespace fp::wear
