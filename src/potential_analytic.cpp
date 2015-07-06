#include "potential_analytic.h"
#include <cmath>

namespace potential{

    void Plummer::eval_sph_rad(const coord::PosSph &pos,
        double* potential, double* deriv, double* deriv2) const
    {
        double rsq = pow_2(pos.r) + pow_2(scaleRadius);
        double pot = -mass/sqrt(rsq);
        if(potential)
            *potential = pot;
        if(deriv)
            *deriv = -pot*pos.r/rsq;
        if(deriv2)
            *deriv2 = pot*(2*pow_2(pos.r)-pow_2(scaleRadius))/pow_2(rsq);
    }
#if 0
    double Plummer::density_sph(const coord::PosSph &pos) const
    {
        return 3./(4*M_PI)*mass/pow_3(scaleRadius)*pow(1+pow_2(pos.r/scaleRadius), -2.5); 
    }
#endif

    void NFW::eval_sph_rad(const coord::PosSph &pos,
        double* potential, double* deriv, double* deriv2) const
    {
        double ln = log(1 + pos.r/scaleRadius);
        if(potential)
            *potential = -mass/pos.r*ln;
        if(deriv)
            *deriv = mass * (pos.r==0 ? 0.5/pow_2(scaleRadius) :
                (ln/pos.r - 1/(pos.r+scaleRadius))/pos.r );
        if(deriv2)
            *deriv2 = -mass * (pos.r==0 ? 2./(3*scaleRadius*pow_2(scaleRadius)) :
                (2*ln/pos.r - (2*scaleRadius+3*pos.r)/pow_2(scaleRadius+pos.r) )/pow_2(pos.r) );
    }

    void MiyamotoNagai::eval_cyl(const coord::PosCyl &pos,
        double* potential, coord::GradCyl* deriv, coord::HessCyl* deriv2) const
    {
        double zb=sqrt(pow_2(pos.z)+pow_2(scaleRadiusB));
        double azb2=pow_2(scaleRadiusA+zb);
        double denom=1/sqrt(pow_2(pos.R) + azb2);
        if(potential)
            *potential = -mass*denom;
        if(deriv) {
            double denom3=mass*denom*denom*denom;
            deriv->dR = pos.R * denom3;
            deriv->dz = pos.z * denom3 * (1 + scaleRadiusA/zb);
            deriv->dphi = 0;
        }
        if(deriv2) {
            double denom5=mass*pow_2(pow_2(denom))*denom;
            deriv2->dR2 = denom5 * (azb2 - 2*pow_2(pos.R));
            deriv2->dz2 = denom5 *( (pow_2(pos.R) - 2*azb2) * pow_2(pos.z/zb) +
                pow_2(scaleRadiusB) * (scaleRadiusA/zb+1) * (pow_2(pos.R) + azb2) / pow_2(zb) );
            deriv2->dRdz= denom5 * -3*pos.R*pos.z * (scaleRadiusA/zb + 1);
            deriv2->dRdphi = deriv2->dzdphi = deriv2->dphi2 = 0;
        }
    }

    void Logarithmic::eval_car(const coord::PosCar &pos,
        double* potential, coord::GradCar* deriv, coord::HessCar* deriv2) const
    {
        double m2 = coreRadius2 + pow_2(pos.x) + pow_2(pos.y)/q2 + pow_2(pos.z)/p2;
        if(potential)
            *potential = sigma2*log(m2)*0.5;
        if(deriv) {
            deriv->dx = pos.x*sigma2/m2;
            deriv->dy = pos.y*sigma2/m2/q2;
            deriv->dz = pos.z*sigma2/m2/p2;
        }
        if(deriv2) {
            deriv2->dx2 = sigma2*(1/m2    - 2*pow_2(pos.x/m2));
            deriv2->dy2 = sigma2*(1/m2/q2 - 2*pow_2(pos.y/(m2*q2)));
            deriv2->dz2 = sigma2*(1/m2/p2 - 2*pow_2(pos.z/(m2*p2)));
            deriv2->dxdy=-sigma2*pos.x*pos.y * 2/(pow_2(m2)*q2);
            deriv2->dydz=-sigma2*pos.y*pos.z * 2/(pow_2(m2)*q2*p2);
            deriv2->dxdz=-sigma2*pos.z*pos.x * 2/(pow_2(m2)*p2);
        }
    }

}  // namespace potential