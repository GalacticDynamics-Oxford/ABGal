#include "galaxymodel_spherical.h"
#include "math_core.h"
#include "math_sample.h"
#include "math_specfunc.h"
#include "utils.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <fstream>

namespace galaxymodel{

namespace{

/// required tolerance for the root-finder
static const double EPSROOT  = 1e-6;

/// tolerance on the 2nd derivative of a function of phase volume for grid generation
static const double ACCURACY_INTERP  = ROOT3_DBL_EPSILON;

/// fixed order of Gauss-Legendre quadrature on each segment of the grid
static const int GLORDER  = 8;    // default value for all segments, or, alternatively, two values:
static const int GLORDER1 = 6;    // for shorter segments
static const int GLORDER2 = 10;   // for larger segments
/// the choice between short and long segments is determined by the segment length in log(h)
static const double GLDELTA = 0.7;  // ln(2)

/// lower limit on the value of density or DF to be considered seriously
/// (this weird-looking threshold takes into account possible roundoff errors
/// in converting the values to/from log-scaled ones,
static const double MIN_VALUE_ROUNDOFF = 0.9999999999999e-100;


/// integrand for computing the product of f(E) and a weight function (E-Phi)^{P/2}
template<int P>
class SphericalIsotropicDFIntegrand: public math::IFunctionNoDeriv {
    const math::IFunction &df;
    const potential::PhaseVolume &pv;
    const double logh0;
public:
    SphericalIsotropicDFIntegrand(
        const math::IFunction& _df, const potential::PhaseVolume& _pv, double _logh0) :
        df(_df), pv(_pv), logh0(_logh0) {}

    virtual double value(const double logh) const
    {
        double h = exp(logh), g, w = sqrt(pv.deltaE(logh, logh0, &g));
        // the original integrals are formulated in terms of  \int f(E) weight(E) dE,
        // and we replace  dE  by  d(log h) * [ dh / d(log h) ] / [ dh / dE ],
        // that's why there are extra factors h and 1/g below.
        return df(h) * h / g * math::pow(w, P);
    }
};


/// helper class for integrating or sampling the isotropic DF in a given spherical potential
class SphericalIsotropicModelIntegrand: public math::IFunctionNdim {
    const math::IFunction& pot;
    const math::IFunction& df;
    const potential::PhaseVolume pv;
public:
    SphericalIsotropicModelIntegrand(const math::IFunction& _pot, const math::IFunction& _df) :
        pot(_pot), df(_df), pv(pot) {}

    /// un-scale r and v and return the jacobian of this transformation
    double unscalerv(double scaledr, double scaledv, double& r, double& v, double& Phi) const {
        double drds;
        r   = math::unscale(math::ScalingSemiInf(), scaledr, &drds);
        Phi = pot(r);
        double vesc = sqrt(-2*Phi);
        v   = scaledv * vesc;
        return pow_2(4*M_PI) * pow_2(r * vesc * scaledv) * vesc * drds;
    }

    virtual void eval(const double vars[], double values[]) const
    {
        double r, v, Phi;
        double jac = unscalerv(vars[0], vars[1], r, v, Phi);
        values[0] = 0;
        if(isFinite(jac) && jac>1e-100 && jac<1e100) {
            double f = df(pv(Phi + 0.5 * v*v));
            if(isFinite(f))
                values[0] = f * jac;
        }
    }
    virtual unsigned int numVars()   const { return 2; }
    virtual unsigned int numValues() const { return 1; }
};


/// Helper class for finding the value of energy at which
/// the cumulative distribution function equals the target value
class VelocitySampleRootFinder: public math::IFunctionNoDeriv {
    const SphericalIsotropicModel& model; ///< the model providing h(E) and J0(h)
    const math::CubicSpline2d& intJ1;     ///< J1 as a function of h(E) and h(Phi)
    const double Phi;                     ///< Phi, the potential at the given radius
    const double loghPhi;                 ///< log(h(Phi)) is cached to avoid its repeated evaluation
    const double I0plusJ0;                ///< I0(h(Phi))
    const double target;                  ///< target value of the cumulative DF
public:
    VelocitySampleRootFinder(const SphericalIsotropicModel& _model, const math::CubicSpline2d& _intJ1,
        const double _Phi, const double _loghPhi, const double _I0plusJ0, const double _target)
    :
        model(_model), intJ1(_intJ1), Phi(_Phi), loghPhi(_loghPhi), I0plusJ0(_I0plusJ0), target(_target)
    {}
    double value(const double loghEoverhPhi) const
    {
        double hE = exp(loghEoverhPhi + loghPhi);
        double E  = model.phasevol.E(hE);
        double J0 = I0plusJ0 - model.I0(hE);
        double J1 = exp(intJ1.value(loghPhi, loghEoverhPhi)) * J0;
        double val= J1 * sqrt(fmax(E-Phi, 0.));
        return val - target;
    }
};


}  // internal namespace


//---- create an N-body realization of a spherical model ----//

particles::ParticleArraySph samplePosVel(
    const math::IFunction& pot, const math::IFunction& df, const unsigned int numPoints)
{
    SphericalIsotropicModelIntegrand fnc(pot, df);
    math::Matrix<double> result;  // sampled scaled coordinates/velocities
    double totalMass, errorMass;  // total normalization of the DF and its estimated error
    double xlower[2] = {0,0};     // boundaries of sampling region in scaled coordinates
    double xupper[2] = {1,1};
    math::sampleNdim(fnc, xlower, xupper, numPoints, result, NULL, &totalMass, &errorMass);
    const double pointMass = totalMass / result.rows();
    particles::ParticleArraySph points;
    points.data.reserve(result.rows());
    for(unsigned int i=0; i<result.rows(); i++) {
        double r, v, Phi, vdir[3],
        rtheta = acos(math::random()*2-1),
        rphi   = 2*M_PI * math::random();
        math::getRandomUnitVector(vdir);
        fnc.unscalerv(result(i, 0), result(i, 1), r, v, Phi);
        points.add(coord::PosVelSph(r, rtheta, rphi, v*vdir[0], v*vdir[1], v*vdir[2]), pointMass);
    }
    return points;
}


//---- Compute density and optionally velocity dispersion from DF ----//

std::vector<double> computeDensity(const math::IFunction& df, const potential::PhaseVolume& pv,
    const std::vector<double> &gridPhi, std::vector<double> *gridVelDisp)
{
    unsigned int gridsize = gridPhi.size();
    std::vector<double> result(gridsize);
    if(gridVelDisp)
        gridVelDisp->assign(gridsize, 0);
    // assuming that the grid in Phi is sufficiently dense, use a fixed-order quadrature on each segment
    const double *glnodes = math::GLPOINTS[GLORDER], *glweights = math::GLWEIGHTS[GLORDER];
    for(unsigned int i=0; i<gridsize; i++) {
        double deltaPhi = (i<gridsize-1 ? gridPhi[i+1] : 0) - gridPhi[i];
        if(deltaPhi<=0)
            throw std::runtime_error("computeDensity: grid in Phi must be monotonically increasing");
        for(int k=0; k<GLORDER; k++) {
            // node of Gauss-Legendre quadrature within the current segment (Phi[i] .. Phi[i+1]);
            // the integration variable y ranges from 0 to 1, and Phi(y) is defined below
            double y   = glnodes[k];
            double Phi = gridPhi[i] + y*y * deltaPhi;
            // contribution of this point to each integral on the current segment, taking into account
            // the transformation of variable y -> Phi, multiplied by the value of f(h(Phi))
            double weight = glweights[k] * 2*y * deltaPhi * df(pv(Phi)) * (4*M_PI*M_SQRT2);
            // add a contribution to the integrals expressing rho(Phi[j]) for all Phi[j] < Phi
            for(unsigned int j=0; j<=i; j++) {
                double dif = Phi - gridPhi[j];  // guaranteed to be positive (or zero due to roundoff)
                assert(dif>=0);
                double val = dif>0 ? sqrt(dif) * weight : 0;
                result[j] += val;
                if(gridVelDisp)
                    gridVelDisp->at(j) += val * dif;
            }
        }
    }
    if(gridVelDisp)
        for(unsigned int i=0; i<gridsize; i++)
            gridVelDisp->at(i) = sqrt(2./3 * gridVelDisp->at(i) / result[i]);
    return result;
}


//---- Compute projected density and velocity dispersion ----//

void computeProjectedDensity(const math::IFunction& dens, const math::IFunction& velDisp,
    const std::vector<double> &gridR,
    std::vector<double>& gridProjDensity, std::vector<double>& gridProjVelDisp)
{
    unsigned int gridsize = gridR.size();
    gridProjDensity.assign(gridsize, 0);
    gridProjVelDisp.assign(gridsize, 0);
    // assuming that the grid in R is sufficiently dense, use a fixed-order quadrature on each segment
    const double *glnodes = math::GLPOINTS[GLORDER], *glweights = math::GLWEIGHTS[GLORDER];
    for(unsigned int i=0; i<gridsize; i++) {
        bool last = i==gridsize-1;
        double deltar = last ? gridR[i] : gridR[i+1]-gridR[i];
        if(deltar<=0)
            throw std::runtime_error("computeProjectedDensity: grid in R must be monotonically increasing");
        for(int k=0; k<GLORDER; k++) {
            // node of Gauss-Legendre quadrature within the current segment (R[i] .. R[i+1]);
            // the integration variable y ranges from 0 to 1, and r(y) is defined below
            // (differently for the last grid segment which extends to infinity)
            double y = glnodes[k];
            double r = last ? gridR[i] / (1 - y*y) : gridR[i] + y*y * deltar;
            // contribution of this point to each integral on the current segment, taking into account
            // the transformation of variable y -> r, multiplied by the value of rho(r)
            double weight = glweights[k] * (last ? 2*y / pow_2(1-y*y) : 2*y) * deltar * dens(r) * 2*r;
            double velsq  = pow_2(velDisp(r));
            // add a contribution to the integrals expressing Sigma(R) for all R[j] < r
            for(unsigned int j=0; j<=i; j++) {
                double dif = pow_2(r) - pow_2(gridR[j]);  // guaranteed to be positive
                assert(dif>0);
                double val = weight / sqrt(dif);
                gridProjDensity[j] += val;
                gridProjVelDisp[j] += val * velsq;
            }
        }
    }
    for(unsigned int i=0; i<gridsize; i++)
        gridProjVelDisp[i] = sqrt(gridProjVelDisp[i] / gridProjDensity[i]);
}


//---- Spherical model specified by a DF f(h) and phase volume h(E) ----//

SphericalIsotropicModel::SphericalIsotropicModel(
    const potential::PhaseVolume& _phasevol, const math::IFunction& df, const std::vector<double>& gridh)
:
    phasevol(_phasevol)
{
    // 1. determine the range of h that covers the region of interest
    // and construct the grid in log[h(Phi)] if it wasn't provided
    std::vector<double> gridLogH;
    if(gridh.empty())
        gridLogH = math::createInterpolationGrid(math::LogLogScaledFnc(df), ACCURACY_INTERP);
    else {
        gridLogH.resize(gridh.size());
        for(size_t i=0; i<gridh.size(); i++)
            gridLogH[i] = log(gridh[i]);
    }
    const unsigned int npoints = gridLogH.size();

    // 2. store the values of f, g, h at grid nodes (ensure to consider only positive values of f)
    std::vector<double> gridF(npoints), gridG(npoints), gridH(npoints), gridE(npoints);
    for(unsigned int i=0; i<npoints; i++) {
        double h = exp(gridLogH[i]);
        double f = df(h);
        if(!(f>=0))
            throw std::runtime_error("SphericalIsotropicModel: "
                "f("+utils::toString(h)+")="+utils::toString(f));
        gridF[i] = f;
        gridH[i] = h;
        gridE[i] = phasevol.E(h, &gridG[i]);
    }
    std::vector<double> gridFint(npoints), gridFGint(npoints), gridFHint(npoints), gridFEint(npoints);

    // 3a. determine the asymptotic behaviour of f(h):
    // f(h) ~ h^outerFslope as h-->inf  or  h^innerFslope as h-->0
    double innerFslope, outerFslope;
    if(df.numDerivs() >= 1) {
        double der;
        df.evalDeriv(gridH[0], NULL, &der);
        innerFslope = der / gridF[0] * gridH[0];
        df.evalDeriv(gridH[npoints-1], NULL, &der);
        outerFslope = der / gridF[npoints-1] * gridH[npoints-1];
    } else {
        innerFslope = log(gridF[1] / gridF[0]) / (gridLogH[1] - gridLogH[0]);
        outerFslope = log(gridF[npoints-1] / gridF[npoints-2]) /
            (gridLogH[npoints-1] - gridLogH[npoints-2]);
    }
    if(gridF[0] <= MIN_VALUE_ROUNDOFF) {
        gridF[0] = innerFslope = 0.;
    } else if(!(innerFslope > -1))
        throw std::runtime_error("SphericalIsotropicModel: f(h) rises too rapidly as h-->0\n"
            "f(h="+utils::toString(gridH[0])+")="+utils::toString(gridF[0]) + "; "
            "f(h="+utils::toString(gridH[1])+")="+utils::toString(gridF[1]) + " => "
            "f ~ h^"+utils::toString(innerFslope));
    if(gridF[npoints-1] <= MIN_VALUE_ROUNDOFF) {
        gridF[npoints-1] = outerFslope = 0.;
    } else if(!(outerFslope < -1))
        throw std::runtime_error("SphericalIsotropicModel: f(h) falls off too slowly as h-->infinity\n"
             "f(h="+utils::toString(gridH[npoints-1])+")="+utils::toString(gridF[npoints-1]) + "; "
             "f(h="+utils::toString(gridH[npoints-2])+")="+utils::toString(gridF[npoints-2]) + " => "
             "f ~ h^"+utils::toString(outerFslope));

    // 3b. determine the asymptotic behaviour of h(E), or rather, g(h) = dh/dE:
    // -E ~ h^outerEslope  and  g(h) ~ h^(1-outerEslope)  as  h-->inf,
    // and in the nearly Keplerian potential at large radii outerEslope should be ~ -2/3.
    // -E ~ h^innerEslope + const  and  g(h) ~ h^(1-innerEslope)  as  h-->0:
    // if innerEslope<0, Phi(r) --> -inf as r-->0, and we assume that |innerE| >> const;
    // otherwise Phi(0) is finite, and we assume that  innerE-Phi(0) << |Phi(0)|.
    // in general, if Phi ~ r^n + const at small r, then innerEslope = 2n / (6+3n);
    // innerEslope ranges from -2/3 for a Kepler potential to ~0 for a logarithmic potential,
    // to +1/3 for a harmonic (constant-density) core.
    double Phi0   = phasevol.E(0);  // Phi(r=0), may be -inf
    double innerE = gridE.front();
    double outerE = gridE.back();
    if(!(Phi0 < innerE && innerE < outerE && outerE < 0))
        throw std::runtime_error("SphericalIsotropicModel: weird behaviour of potential\n"
            "Phi(0)="+utils::toString(Phi0)  +", "
            "innerE="+utils::toString(innerE)+", "
            "outerE="+utils::toString(outerE));
    if(Phi0 != -INFINITY)   // determination of inner slope depends on whether the potential is finite
        innerE -= Phi0;
    double innerEslope = gridH.front() / gridG.front() / innerE;
    double outerEslope = gridH.back()  / gridG.back()  / outerE;
    double outerRatio  = outerFslope  / outerEslope;
    if(!(outerEslope < 0))   // should be <0 if the potential tends to zero at infinity
        throw std::runtime_error("SphericalIsotropicModel: weird behaviour of E(h) at infinity: "
            "E ~ h^" +utils::toString(outerEslope));
    if(!(innerEslope + innerFslope > -1))
        throw std::runtime_error("SphericalIsotropicModel: weird behaviour of f(h) at origin: "
            "E ~ h^"+utils::toString(innerEslope)+", "
            "f ~ h^"+utils::toString(innerFslope)+", "
            "their product grows faster than h^-1 => total energy is infinite");

    // 4. compute integrals
    // \int f(E) dE        = \int f(h) / g(h) h d(log h),    [?]
    // \int f(E) g(E) dE   = \int f(h) h d(log h),           [mass]
    // \int f(E) h(E) dE   = \int f(h) / g(h) h^2 d(log h),  [kinetic energy]
    // \int f(E) g(E) E dE = \int f(h) E h d(log h)          [total energy]

    // 4a. integrate over all interior segments
    const double *glnodes1 = math::GLPOINTS[GLORDER1], *glweights1 = math::GLWEIGHTS[GLORDER1];
    const double *glnodes2 = math::GLPOINTS[GLORDER2], *glweights2 = math::GLWEIGHTS[GLORDER2];
    for(unsigned int i=1; i<npoints; i++) {
        double dlogh = gridLogH[i]-gridLogH[i-1];
        // choose a higher-order quadrature rule for longer grid segments
        int glorder  = dlogh < GLDELTA ? GLORDER1 : GLORDER2;
        const double *glnodes   = glorder == GLORDER1 ? glnodes1   : glnodes2;
        const double *glweights = glorder == GLORDER1 ? glweights1 : glweights2;
        for(int k=0; k<glorder; k++) {
            // node of Gauss-Legendre quadrature within the current segment (logh[i-1] .. logh[i]);
            double logh = gridLogH[i-1] + dlogh * glnodes[k];
            // GL weight -- contribution of this point to each integral on the current segment
            double weight = glweights[k] * dlogh;
            // compute E, f, g, h at the current point h (GL node)
            double h = exp(logh), g, E = phasevol.E(h, &g), f = df(h);
            if(!(f>=0))
                throw std::runtime_error("SphericalIsotropicModel: "
                    "f("+utils::toString(h)+")="+utils::toString(f));
            // the original integrals are formulated in terms of  \int f(E) weight(E) dE,
            // where weight = 1, g, h for the three integrals,
            // and we replace  dE  by  d(log h) * [ dh / d(log h) ] / [ dh / dE ],
            // that's why there are extra factors h and 1/g below.
            double integrand = f * h * weight;
            gridFint[i-1] += integrand / g;
            gridFGint[i]  += integrand;
            gridFHint[i]  += integrand / g * h;
            gridFEint[i]  -= integrand * E;
        }
    }

    // 4b. integral of f(h) dE = f(h) / g(h) dh -- compute from outside in,
    // summing contributions from all intervals of h above its current value
    // the outermost segment from h_max to infinity is integrated analytically
    gridFint.back() = -gridF.back() * outerE / (1 + outerRatio);
    for(int i=npoints-1; i>=1; i--) {
        gridFint[i-1] += gridFint[i];
    }

    // 4c. integrands of f*g dE,  f*h dE  and  f*g*E dE;  note that g = dh/dE.
    // compute from inside out, summing contributions from all previous intervals of h
    // integrals over the first segment (0..gridH[0]) are computed analytically
    gridFGint[0] = gridF[0] * gridH[0] / (1 + innerFslope);
    gridFHint[0] = gridF[0] * pow_2(gridH[0]) / gridG[0] / (1 + innerEslope + innerFslope);
    gridFEint[0] = gridF[0] * gridH[0] * (innerEslope >= 0 ?
        -Phi0   / (1 + innerFslope) :
        -innerE / (1 + innerFslope + innerEslope) );

    for(unsigned int i=1; i<npoints; i++) {
        gridFGint[i] += gridFGint[i-1];
        gridFHint[i] += gridFHint[i-1];
        gridFEint[i] += gridFEint[i-1];
    }
    // add the contribution of integrals from the last grid point up to infinity (very small anyway)
    gridFGint.back() -= gridF.back() * gridH.back() / (1 + outerFslope);
    gridFHint.back() -= gridF.back() * pow_2(gridH.back()) / gridG.back() / (1 + outerEslope + outerFslope);
    gridFEint.back() += gridF.back() * gridH.back() * outerE / (1 + outerEslope + outerFslope);
    totalMass = gridFGint.back();
    if(!(totalMass > 0))
        throw std::runtime_error("SphericalIsotropicModel: f(h) is nowhere positive");

    // decide on the value of h separating two regimes of computing f(h) from interpolating splines:
    // if h is not too large, use intfg, otherwise use intf
    htransition = gridH[0];
    for(unsigned int i=1; i<npoints-1 && gridFGint[i+1] < totalMass * 0.999; i++)
        htransition = gridH[i];

    // 5. construct 1d interpolating splines for these integrals
    // 5a. prepare derivatives for quintic spline
    std::vector<double> gridFder(npoints), gridFGder(npoints), gridFHder(npoints), gridFEder(npoints);
    for(unsigned int i=0; i<npoints; i++) {
        gridFder [i] = -gridF[i] / gridG[i];
        gridFGder[i] =  gridF[i];
        gridFHder[i] =  gridF[i] * gridH[i] / gridG[i];
        gridFEder[i] = -gridF[i] * gridE[i];
        if(!(gridFder[i]<=0 && gridFGder[i]>=0 && gridFHder[i]>=0 && gridFEder[i]>=0 && 
            isFinite(gridFint[i] + gridFGint[i] + gridFHint[i] + gridFEint[i])))
            throw std::runtime_error("SphericalIsotropicModel: cannot construct valid interpolators");
    }
    // integrals of f*g, f*h and f*g*E have finite limit as h-->inf;
    // extrapolate them as constants beyond the last grid point
    gridFGder.back() = gridFHder.back() = gridFEder.back() = 0;

    // debugging output
    if(utils::verbosityLevel >= utils::VL_VERBOSE) {
        std::ofstream strm("SphericalIsotropicModel.log");
        strm << "h             \tg             \tE             \tf(E)          \t"
            "int_E^0 f dE  \tint_Phi0^E f g\tint_Phi0^E f h\tint_Phi0^E f g E\n";
        for(unsigned int i=0; i<npoints; i++) {
            strm <<
            utils::pp(gridH[i],     14) + '\t' + utils::pp(gridG[i],     14) + '\t' +
            utils::pp(gridE[i],     14) + '\t' + utils::pp(gridF[i],     14) + '\t' +
            utils::pp(gridFint[i],  14) + '\t' + utils::pp(gridFGint[i], 14) + '\t' +
            utils::pp(gridFHint[i], 14) + '\t' + utils::pp(gridFEint[i], 14) + '\n';
        }
    }

    // 5b. initialize splines for log-scaled integrals
    intf  = math::LogLogSpline(gridH, gridFint,  gridFder);
    intfg = math::LogLogSpline(gridH, gridFGint, gridFGder);
    intfh = math::LogLogSpline(gridH, gridFHint, gridFHder);
    intfE = math::LogLogSpline(gridH, gridFEint, gridFEder);
}

void SphericalIsotropicModel::evalDeriv(const double h, double* f, double* dfdh, double* /*ignored*/) const
{
    double der, der2, g, dgdh;
    // at large h, intfg reaches a limit (totalMass), thus its derivative may be inaccurate
    if(h < htransition) {   // still ok
        // f(h) = d[ int_0^h f(h') dh' ] / d h
        intfg.evalDeriv(h, NULL, &der, dfdh? &der2 : NULL);
        if(f)
            *f = der;
        if(dfdh)
            *dfdh = der2;
    } else {
        // otherwise we compute it from a different spline which tends to zero at large h:
        // f(h) = -g(h)  d[ int_h^\infty f(h') / g(h') dh' ] / d h
        intf.evalDeriv(h, NULL, &der, dfdh? &der2 : NULL);
        phasevol.E(h, &g, dfdh? &dgdh : NULL);
        if(f)
            *f = -der * g;
        if(dfdh)
            *dfdh = -der2 * g - der * dgdh;
    }
}

double SphericalIsotropicModel::I0(const double h) const
{
    return intf(h);
}

double SphericalIsotropicModel::cumulMass(const double h) const
{
    if(h==INFINITY)
        return totalMass;
    return intfg(h);
}

double SphericalIsotropicModel::cumulEkin(const double h) const
{
    return 1.5 * intfh(h);
}

double SphericalIsotropicModel::cumulEtotal(const double h) const
{
    return -intfE(h);
}

//---- Extended spherical model with 2d interpolation for position-dependent quantities ----//

void SphericalIsotropicModelLocal::init(const math::IFunction& df, const std::vector<double>& gridh)
{
    // 1. determine the range of h that covers the region of interest
    // and construct the grid in X = log[h(Phi)] and Y = log[h(E)/h(Phi)]
    std::vector<double> gridLogH;
    if(gridh.empty())
        gridLogH = math::createInterpolationGrid(math::LogLogScaledFnc(df), ACCURACY_INTERP);
    else {
        gridLogH.resize(gridh.size());
        for(size_t i=0; i<gridh.size(); i++)
            gridLogH[i] = log(gridh[i]);
    }
    while(!gridLogH.empty() && df(exp(gridLogH.back())) <= MIN_VALUE_ROUNDOFF)  // ensure that f(hmax)>0
        gridLogH.pop_back();
    if(gridLogH.size() < 3)
        throw std::runtime_error("SphericalIsotropicModelLocal: f(h) is nowhere positive");
    const double logHmin        = gridLogH.front(),  logHmax = gridLogH.back();
    const unsigned int npoints  = gridLogH.size();
    const unsigned int npointsY = 100;
    const double mindeltaY      = fmin(0.1, (logHmax-logHmin)/npointsY);
    std::vector<double> gridY   = math::createNonuniformGrid(npointsY, mindeltaY, logHmax-logHmin, true);

    // 3. determine the asymptotic behaviour of f(h) and g(h):
    // f(h) ~ h^outerFslope as h-->inf and  g(h) ~ h^(1-outerEslope)
    double outerH = exp(gridLogH.back()), outerG;
    double outerE = phasevol.E(outerH, &outerG), outerFslope;
    if(df.numDerivs() >= 1) {
        double val, der;
        df.evalDeriv(outerH, &val, &der);
        outerFslope = der / val * outerH;
    } else {
        outerFslope = log(df(outerH) / df(exp(gridLogH[npoints-2]))) /
            (gridLogH[npoints-1] - gridLogH[npoints-2]);
    }
    if(!(outerFslope < -1))
        // in this case SphericalIsotropicModel would have already thrown the same exception
        throw std::runtime_error("SphericalIsotropicModelLocal: "
            "f(h) falls off too slowly as h-->infinity");
    double outerEslope = outerH / outerG / outerE;
    double outerRatio  = outerFslope / outerEslope;
    if(!(outerRatio > 0))
        throw std::runtime_error("SphericalIsotropicModelLocal: "
            "weird asymptotic behaviour of phase volume\n"
            "h(E="+utils::toString(outerE)+")="+utils::toString(outerH) +
            "; dh/dE="+utils::toString(outerG) + " => outerEslope="+utils::toString(outerEslope) +
            ", outerFslope="+utils::toString(outerFslope));

    // 5. construct 2d interpolating splines for dv2par, dv2per as functions of Phi and E

    // 5a. asymptotic values for J1/J0 and J3/J0 as Phi --> 0 and (E/Phi) --> 0
    double outerJ1 = 0.5*M_SQRTPI * math::gamma(2 + outerRatio) / math::gamma(2.5 + outerRatio);
    double outerJ3 = outerJ1 * 1.5 / (2.5 + outerRatio);

    // 5b. compute the values of J1/J0 and J3/J0 at nodes of 2d grid in X=log(h(Phi)), Y=log(h(E)/h(Phi))
    math::Matrix<double> gridJ1(npoints, npointsY), gridJ3(npoints, npointsY);
    for(unsigned int i=0; i<npoints; i++)
    {
        // The first coordinate of the grid is X = log(h(Phi)), the second is Y = log(h(E)) - X.
        // For each pair of values of X and Y, we compute the following integrals:
        // J_n = \int_\Phi^E f(E') [(E'-\Phi) / (E-\Phi)]^{n/2}  dE';  n = 0, 1, 3.
        // Then the value of 2d interpolants are assigned as
        // \log[ J3 / J0 ], \log[ (3*J1-J3) / J0 ] .
        // In practice, we replace the integration over dE by integration over dy = d(log h),
        // and accumulate the values of modified integrals sequentially over each segment in Y.
        // Here the modified integrals are  J{n}acc = \int_X^Y f(y) (dE'/dy) (E'(y)-\Phi)^{n/2}  dy,
        // i.e., without the term [E(Y,X)-\Phi(X)]^{n/2} in the denominator,
        // which is invoked later when we assign the values to the 2d interpolants.
        double J0acc = 0, J1acc = 0, J3acc = 0;  // accumulators
        SphericalIsotropicDFIntegrand<0> intJ0(df, phasevol, gridLogH[i]);
        SphericalIsotropicDFIntegrand<1> intJ1(df, phasevol, gridLogH[i]);
        SphericalIsotropicDFIntegrand<3> intJ3(df, phasevol, gridLogH[i]);
        gridJ1(i, 0) = log(2./3);  // analytic limiting values for Phi=E
        gridJ3(i, 0) = log(2./5);
        for(unsigned int j=1; j<npointsY; j++) {
            double logHprev = gridLogH[i] + gridY[j-1];
            double logHcurr = gridLogH[i] + gridY[j];
            if(j==1) {
                // integration over the first segment uses a more accurate quadrature rule
                // to accounting for a possible endpoint singularity at Phi=E
                math::ScalingCub scaling(logHprev, logHcurr);
                J0acc = math::integrateGL(
                    math::ScaledIntegrand<math::ScalingCub>(scaling, intJ0), 0, 1, GLORDER);
                J1acc = math::integrateGL(
                    math::ScaledIntegrand<math::ScalingCub>(scaling, intJ1), 0, 1, GLORDER);
                J3acc = math::integrateGL(
                    math::ScaledIntegrand<math::ScalingCub>(scaling, intJ3), 0, 1, GLORDER);
            } else {
                J0acc += math::integrateGL(intJ0, logHprev, logHcurr, GLORDER);
                J1acc += math::integrateGL(intJ1, logHprev, logHcurr, GLORDER);
                J3acc += math::integrateGL(intJ3, logHprev, logHcurr, GLORDER);
            }
            if(i==npoints-1) {
                // last row: analytic limiting values for Phi-->0 and any E/Phi
                double EoverPhi = exp(gridY[j] * outerEslope);  // strictly < 1
                double oneMinusJ0overI0 = std::pow(EoverPhi, 1+outerRatio);  // < 1
                double Fval1 = math::hypergeom2F1(-0.5, 1+outerRatio, 2+outerRatio, EoverPhi);
                double Fval3 = math::hypergeom2F1(-1.5, 1+outerRatio, 2+outerRatio, EoverPhi);
                double I0    = this->I0(exp(gridLogH[i]));
                double sqPhi = sqrt(-outerE);
                if(isFinite(Fval1+Fval3)) {
                    J0acc = I0 * (1 - oneMinusJ0overI0);
                    J1acc = I0 * (outerJ1 - oneMinusJ0overI0 * Fval1) * sqPhi;
                    J3acc = I0 * (outerJ3 - oneMinusJ0overI0 * Fval3) * pow_3(sqPhi);
                } else {
                    // this procedure sometimes fails, since hypergeom2F1 is not very robust;
                    // in this case we simply keep the values computed by numerical integration
                    utils::msg(utils::VL_WARNING, "SphericalIsotropicModelLocal",
                        "Can't compute asymptotic value");
                }
            }
            double dv = sqrt(phasevol.deltaE(logHcurr, gridLogH[i]));
            double J1overJ0 = J1acc / J0acc / dv;
            double J3overJ0 = J3acc / J0acc / pow_3(dv);
            if(J1overJ0<=0 || J3overJ0<=0 || !isFinite(J1overJ0+J3overJ0)) {
                utils::msg(utils::VL_WARNING, "SphericalIsotropicModelLocal", "Invalid value"
                    "  J0="+utils::toString(J0acc)+
                    ", J1="+utils::toString(J1acc)+
                    ", J3="+utils::toString(J3acc));
                J1overJ0 = 2./3;   // fail-safe values corresponding to E=Phi
                J3overJ0 = 2./5;
            }
            gridJ1(i, j) = log(J1overJ0);
            gridJ3(i, j) = log(J3overJ0);
        }
    }

    // debugging output
    if(utils::verbosityLevel >= utils::VL_VERBOSE) {
        std::ofstream strm("SphericalIsotropicModelLocal.log");
        strm << "ln[h(Phi)] ln[hE/hPhi]\tPhi            E             \tJ1         J3\n";
        for(unsigned int i=0; i<npoints; i++) {
            double Phi = phasevol.E(exp(gridLogH[i]));
            for(unsigned int j=0; j<npointsY; j++) {
                double E = phasevol.E(exp(gridLogH[i] + gridY[j]));
                strm << utils::pp(gridLogH[i],10) +' '+ utils::pp(gridY[j],10) +'\t'+
                utils::pp(Phi,14) +' '+ utils::pp(E,14) +'\t'+
                utils::pp(exp(gridJ1(i, j)),10) +' '+ utils::pp(exp(gridJ3(i, j)),10)+'\n';
            }
            strm << '\n';
        }
    }

    // 5c. construct the 2d splines
    intJ1 = math::CubicSpline2d(gridLogH, gridY, gridJ1);
    intJ3 = math::CubicSpline2d(gridLogH, gridY, gridJ3);
}

void SphericalIsotropicModelLocal::evalLocal(
    double Phi, double E, double &dvpar, double &dv2par, double &dv2per) const
{
    double hPhi = phasevol(Phi);
    double hE   = phasevol(E);
    if(!(Phi<0 && hE >= hPhi))
        throw std::invalid_argument("SphericalIsotropicModelLocal: incompatible values of E and Phi");

    // compute the 1d interpolators for I0, J0
    double I0 = this->I0(hE);
    double J0 = fmax(this->I0(hPhi) - I0, 0);
    // restrict the arguments of 2d interpolators to the range covered by their grids
    double X  = math::clamp(log(hPhi),    intJ1.xmin(), intJ1.xmax());
    double Y  = math::clamp(log(hE/hPhi), intJ1.ymin(), intJ1.ymax());
    // compute the 2d interpolators for J1, J3
    double J1 = exp(intJ1.value(X, Y)) * J0;
    double J3 = exp(intJ3.value(X, Y)) * J0;
    if(E>=0) {  // in this case, the coefficients were computed for E=0, need to scale them to E>0
        double corr = 1 / sqrt(1 - E / Phi);  // correction factor <1
        J1 *= corr;
        J3 *= pow_3(corr);
    }
    double mult = 32*M_PI*M_PI/3 * cumulMass();
    dvpar  = -mult *  J1 * 3;
    dv2par =  mult * (I0 + J3);
    dv2per =  mult * (I0 * 2 + J1 * 3 - J3);
    /*if(loghPhi<X)
        utils::msg(utils::VL_WARNING, "SphericalIsotropicModelLocal",
        "Extrapolating to small h: log(h(Phi))="+utils::toString(loghPhi)+
        ", log(h(E))="+utils::toString(loghE)+
        ", I0="+utils::toString(I0)+", J0="+utils::toString(J0));*/
}

double SphericalIsotropicModelLocal::sampleVelocity(double Phi, math::PRNGState* state) const
{
    if(!(Phi<0))
        throw std::invalid_argument("SphericalIsotropicModelLocal: invalid value of Phi");
    double hPhi     = phasevol(Phi);
    double loghPhi  = math::clamp(log(hPhi), intJ1.xmin(), intJ1.xmax());
    double I0plusJ0 = I0(hPhi);
    double maxJ1    = exp(intJ1.value(loghPhi, intJ1.ymax())) * I0plusJ0;
    double frac     = math::random(state);
    double target   = frac * maxJ1 * sqrt(-Phi);
    // find the value of E at which the cumulative distribution function equals the target
    double loghEoverhPhi = math::findRoot(
        VelocitySampleRootFinder(*this, intJ1, Phi, loghPhi, I0plusJ0, target),
        intJ1.ymin(), intJ1.ymax(), EPSROOT);
    if(!(loghEoverhPhi>=0))
       return 0.;  // might not be able to find the root in some perverse cases at very large radii
    double hE = exp(loghEoverhPhi + loghPhi);
    double E  = phasevol.E(hE);
    return sqrt(2. * (E - Phi));
}

double SphericalIsotropicModelLocal::density(double Phi) const
{
    if(!(Phi<0))
        throw std::invalid_argument("SphericalIsotropicModelLocal: invalid value of Phi");
    double hPhi     = phasevol(Phi);
    double loghPhi  = math::clamp(log(hPhi), intJ1.xmin(), intJ1.xmax());
    double J1overJ0 = exp(intJ1.value(loghPhi, intJ1.ymax()));
    double I0plusJ0 = I0(hPhi);  // in fact I0(E)=0 because E=0
    return 4*M_PI*M_SQRT2 * sqrt(-Phi) * J1overJ0 * I0plusJ0;
}

double SphericalIsotropicModelLocal::velDisp(double Phi) const
{
    if(!(Phi<0))
        throw std::invalid_argument("SphericalIsotropicModelLocal: invalid value of Phi");
    double hPhi     = phasevol(Phi);
    double loghPhi  = math::clamp(log(hPhi), intJ1.xmin(), intJ1.xmax());
    double J3overJ1 = exp(intJ3.value(loghPhi, intJ3.ymax()) - intJ1.value(loghPhi, intJ1.ymax()));
    return sqrt(-2./3 * Phi * J3overJ1);
}

//---- non-member functions for various diffusion coefficients ----//

void difCoefEnergy(const SphericalIsotropicModel& model, double E, double &DeltaE, double &DeltaE2)
{
    double h, g;
    model.phasevol.evalDeriv(E, &h, &g);
    double totalMass = model.cumulMass(),
    IF   = model.I0(h),
    IFG  = model.cumulMass(h),
    IFH  = model.cumulEkin(h) * (2./3);
    DeltaE  = 16*M_PI*M_PI * totalMass * (IF - IFG / g);
    DeltaE2 = 32*M_PI*M_PI * totalMass * (IF * h + IFH) / g;
}

double difCoefLosscone(const SphericalIsotropicModel& model, const math::IFunction& pot, double E)
{
    double h = model.phasevol(E), rmax = potential::R_max(pot, E), g, dgdh;
    model.phasevol.E(h, &g, &dgdh);
    // we are computing the orbit-averaged diffusion coefficient  < Delta v_per^2 >,
    // by integrating it over the radial range covered by the orbit.
    // D = [8 pi^2 / g(E)]  int_0^{rmax(E)}  dr  r^2 / v(E,r)  < Delta v_per^2 >,
    // where  < Delta v_per^2 > = 16 pi^2 Mtotal [ 4/3 I_0(E) + 2 J_{1/2}(E,r) - 2/3 J_{3/2}(E,r) ],
    // I_0     = int_E^0  f(E')  dE',
    // J_{n/2} = int_Phi(r)^E  f(E') (v'/v)^n  dE',
    // v(E,r)  = sqrt{ 2 [E - Phi(r)] },  v'(E',r) = sqrt{ 2 [E' - Phi(r)] }.
    // This is a double integral, and the inner integral consists of two parts:
    // (a)  I_0 does not depend on r and may be brought outside the orbit-averaging integral,
    // which itself is computed analytically:
    // int_0^{rmax(E)} dr  r^2 / v  =  1 / (16 pi^2)  dg(E)/dE,  and  dg/dE = g * dg/dh.
    double result = 2./3 * dgdh * model.I0(h);
    // (b)  the remaining terms need to be integrated numerically;
    // we use a fixed-order GL quadrature for both nested integrals
    const double *glnodes = math::GLPOINTS[GLORDER], *glweights = math::GLWEIGHTS[GLORDER];
    for(int ir=0; ir<GLORDER; ir++) {
        // the outermost integral in scaled radial variable:  r/rmax 
        double r = glnodes[ir] * rmax, Phi = pot(r);
        double w = 8*M_PI*M_PI * rmax / g * pow_2(r) * glweights[ir];
        for(int iE=0; iE<GLORDER; iE++) {
            // the innermost integral in scaled energy variable:  (E'-Phi) / (E-Phi)
            double Ep  = E * glnodes[iE] + Phi * (1-glnodes[iE]);
            double fEp = model.value(model.phasevol(Ep));  // model.value is the value of DF
            double vp  = sqrt(2 * (Ep-Phi));
            result += glweights[iE] * w * fEp * vp * (1 - 1./3 * glnodes[iE] /*(Ep-Phi) / (E-Phi)*/);
        }
    }
    return result * 16*M_PI*M_PI * model.cumulMass();
}


// ------ write a text file with various quantities describing a spherical isotropic model ------ //

void writeSphericalIsotropicModel(const std::string& fileName, const std::string& header,
    const SphericalIsotropicModel& model, const math::IFunction& pot, const std::vector<double>& gridh)
{
    // construct a suitable grid in h, if not provided
    std::vector<double> gridH(gridh);
    const math::IFunction& df = model;
    if(gridh.empty()) {
        // estimate the range of log(h) where the DF varies considerably
        std::vector<double> gridLogH =
            math::createInterpolationGrid(math::LogLogScaledFnc(df), ACCURACY_INTERP);
        gridH.resize(gridLogH.size());
        for(size_t i=0; i<gridLogH.size(); i++)
            gridH[i] = exp(gridLogH[i]);
    } else if(gridh.size()<2)
        throw std::runtime_error("writeSphericalIsotropicModel: gridh is too small");

    // construct the corresponding grid in E and r
    double Phi0 = pot(0);
    std::vector<double> gridR, gridPhi, gridG;
    for(size_t i=0; i<gridH.size(); ) {
        double g, Phi = model.phasevol.E(gridH[i], &g);
        // avoid closely spaced potential values whose difference is dominated by roundoff errors
        if(Phi > (gridPhi.empty()? Phi0 : gridPhi.back()) * (1-MIN_VALUE_ROUNDOFF)) {
            gridPhi.push_back(Phi);
            gridG.  push_back(g);
            gridR.  push_back(potential::R_max(pot, Phi));
            i++;
        } else {
            gridH.erase(gridH.begin()+i);
        }
    }
    size_t npoints = gridH.size();

    // compute the density and 1d velocity dispersion by integrating over the DF
    std::vector<double> gridRho, gridVelDisp;
    gridRho = computeDensity(model, model.phasevol, gridPhi, &gridVelDisp);
    for(size_t i=0; i<npoints; i++)  // safety measure to avoid problems in log-log-spline
        if(!isFinite(gridRho[i]+gridVelDisp[i]) || gridRho[i]<=MIN_VALUE_ROUNDOFF)
            gridRho[i] = gridVelDisp[i] = MIN_VALUE_ROUNDOFF;

    // construct interpolators for the density and velocity dispersion profiles
    math::LogLogSpline density(gridR, gridRho);
    math::LogLogSpline veldisp(gridR, gridVelDisp);

    // and use them to compute the projected density and velocity dispersion
    std::vector<double> gridProjDensity, gridProjVelDisp;
    computeProjectedDensity(density, veldisp, gridR, gridProjDensity, gridProjVelDisp);

    double mult = 16*M_PI*M_PI * model.cumulMass();  // common factor for diffusion coefs

    // determine the central mass (check if it appears to be non-zero)
    double coef, slope = potential::innerSlope(pot, NULL, &coef);
    double Mbh = fabs(slope + 1.) < 1e-3 ? -coef : 0;

    // prepare for integrating the density in radius to obtain enclosed mass
    const double *glnodes = math::GLPOINTS[GLORDER], *glweights = math::GLWEIGHTS[GLORDER];
    double Mcumul = 0;

    // print the header and the first line for r=0 (commented out)
    std::ofstream strm(fileName.c_str());
    if(!header.empty())
        strm << "#" << header << "\n";
    strm <<
        "#r      \tM(r)    \tE=Phi(r)\trho(r)  \tf(E)    \tM(E)    \th(E)    \tTrad(E) \trcirc(E) \t"
        "Lcirc(E) \tVelDispersion\tVelDispProj\tSurfaceDensity\tDeltaE^2\tMassFlux\tEnergyFlux";
    if(Mbh>0)
        strm << "\tD_RR/R(0)\n#0        Mbh = " << utils::pp(Mbh, 14) << "\t-INFINITY\n";
    else
        strm << "\n#0      \t0       \t" << utils::pp(Phi0, 14) << '\n';

    // output various quantities as functions of r (or E) to the file
    for(unsigned int i=0; i<gridH.size(); i++) {
        double r = gridR[i], f, dfdh, g = gridG[i], h = gridH[i];
        df.evalDeriv(h, &f, &dfdh);
        // integrate the density on the previous segment
        double rprev = i==0 ? 0 : gridR[i-1];
        for(int k=0; k<GLORDER; k++) {
            double rk = rprev + glnodes[k] * (r-rprev);
            Mcumul += (4*M_PI) * (r-rprev) * glweights[k] * pow_2(rk) * density(rk);
        }
        double
        E        = gridPhi[i],
        rho      = gridRho[i],
        intfg    = model.cumulMass(h),    // mass of particles within phase volume < h
        intfh    = model.cumulEkin(h) * (2./3),
        intf     = model.I0(h),
        //DeltaE   = mult *  (intf - intfg / g),
        DeltaE2  = mult *  (intf * h + intfh) / g * 2.,
        FluxM    =-mult * ((intf * h + intfh) * g * dfdh + intfg * f),
        FluxE    = E * FluxM - mult * ( -(intf * h + intfh) * f + intfg * intf),
        rcirc    = potential::R_circ(pot, E),
        Lcirc    = rcirc * potential::v_circ(pot, rcirc),
        Tradial  = g / (4*M_PI*M_PI * pow_2(Lcirc)),
        veldisp  = gridVelDisp[i],
        veldproj = gridProjVelDisp[i],
        Sigma    = gridProjDensity[i],
        DRRoverR = difCoefLosscone(model, pot, E);

        strm << utils::pp(r,        14) +  // [ 1] radius
        '\t' +  utils::pp(Mcumul,   14) +  // [ 2] enclosed mass
        '\t' +  utils::pp(E,        14) +  // [ 3] Phi(r)=E
        '\t' +  utils::pp(rho,      14) +  // [ 4] rho(r)
        '\t' +  utils::pp(f,        14) +  // [ 5] distribution function f(E)
        '\t' +  utils::pp(intfg,    14) +  // [ 6] mass of particles having energy below E
        '\t' +  utils::pp(h,        14) +  // [ 7] phase volume
        '\t' +  utils::pp(Tradial,  14) +  // [ 8] average radial period at the energy E
        '\t' +  utils::pp(rcirc,    14) +  // [ 9] radius of a circular orbit with energy E
        '\t' +  utils::pp(Lcirc,    14) +  // [10] angular momentum of this circular orbit
        '\t' +  utils::pp(veldisp,  14) +  // [11] 1d velocity dispersion at r
        '\t' +  utils::pp(veldproj, 14) +  // [12] line-of-sight velocity dispersion at projected R
        '\t' +  utils::pp(Sigma,    14) +  // [13] surface density at projected R
        '\t' +  utils::pp(DeltaE2,  14) +  // [14] diffusion coefficient <Delta E^2>
        '\t' +  utils::pp(FluxM,    14) +  // [15] flux of particles through the phase volume
        '\t' +  utils::pp(FluxE,    14);   // [16] flux of energy through the phase volume
        if(Mbh>0)  strm <<                 //      in case of a central black hole:
        '\t' +  utils::pp(DRRoverR, 14);   // [17] loss-cone diffusion coef 
        strm << '\n';
    }
}

}  // namespace galaxymodel
