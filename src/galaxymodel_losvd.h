/** \file    galaxymodel_losvd.h
    \brief   Line-of-sight velocity distribution in Schwarzschild/FEM models
    \date    2017
    \author  Eugene Vasiliev

*/
#pragma once
#include "math_geometry.h"
#include "galaxymodel_target.h"

namespace galaxymodel{

/** Representation of a velocity distribution function in terms of Gauss-Hermite expansion */
class GaussHermiteExpansion: public math::IFunctionNoDeriv {
    double Gamma;  ///< overall normalization (amplitude)
    double Center; ///< position of the center of expansion
    double Sigma;  ///< width of the gaussian
    std::vector<double> moments;  ///< values of Gauss-Hermite moments
public:
    /// initialize the function from previously computed coefficients
    GaussHermiteExpansion(const std::vector<double>& coefs,
        double gamma, double center, double sigma) :
        Gamma(gamma), Center(center), Sigma(sigma), moments(coefs) {}

    /// find the best-fit coefficients for a given function.
    /// If the parameters gamma, center and sigma are not provided, they are estimated
    /// by finding the best-fit Gaussian without higher-order terms; in this case
    /// the first three GH moments should be (1,0,0) to within integration accuracy.
    GaussHermiteExpansion(const math::IFunction& fnc,
        unsigned int order, double gamma=NAN, double center=NAN, double sigma=NAN);

    /// evaluate the expansion at the given point
    virtual double value(double x) const;

    /// return the array of Gauss-Hermite coefficients
    inline const std::vector<double>& coefs() const { return moments; }

    inline double gamma()  const { return Gamma;  }  ///< return the overall normalization factor
    inline double center() const { return Center; }  ///< return the center of expansion
    inline double sigma()  const { return Sigma;  }  ///< return the width of the 0th term

    /// return the normalization constant \f$ N_n = \int_{-\infty}^\infty exp(-x^2/2) H_n(x) dx \f$
    static double normn(unsigned int n);

    /// return the integral of the function over the entire real axis
    double norm() const;
};

/** Construct the matrix that converts the velocity distribution represented by its B-spline amplitudes
    into the Gauss-Hermite moments for a single aperture with known parameters of GH expansion.
    \param[in]  N      is the degree of B-spline (0 to 3);
    \param[in]  grid   is the grid in velocity space defining the B-spline;
    \param[in]  order  is the order M of GH expansion (i.e. it has order+1 coefficients h_0..h_M);
    \param[in]  gamma  is the overall normalization factor of the gaussian;
    \param[in]  center is the central point of the gaussian;
    \param[in]  sigma  is the width of the gaussian;
    \return  a matrix G with (order+1) rows and bsplv.numValues() columns.
    To obtain the GH moments, multiply this matrix by the vector of amplitudes for a single aperture.
*/
math::Matrix<double> computeGaussHermiteMatrix(int N, const std::vector<double>& grid,
    unsigned int order, double gamma, double center, double sigma);


/** Definition of a Gaussian point-spread function with the given width and amplitude */
struct GaussianPSF {
    /// width of the gaussian
    double width;
    /// amplitude (in case of several Gaussian components, their amplitudes are expected to sum up to unity)
    double ampl;
    GaussianPSF(double _width=NAN, double _ampl=1.) : width(_width), ampl(_ampl) {}
};

/** Parameters for handling the line-of-sight velocity distributions */
struct LOSVDParams {
    double alpha, beta, gamma;   ///< viewing angles for transforming the intrinsic to projected coords
    std::vector<double> gridx, gridy;     ///< internal grids in X',Y' (image plane coords)
    std::vector<double> gridv;            ///< grid in line-of-sight velocity
    std::vector<GaussianPSF> spatialPSF;  ///< array of spatial point-spread functions
    double velocityPSF;                   ///< width of the gaussian velocity smoothing kernel
    std::vector<math::Polygon> apertures; ///< array of apertures on the image plane
    coord::SymmetryType symmetry;         ///< symmetry of the potential and the orbital shape

    /// set (unreasonable) default values
    LOSVDParams() :
        alpha(0.), beta(0.), gamma(0.), velocityPSF(0.), symmetry(coord::ST_TRIAXIAL) {}
};


/** The class for recording the line-of-sight velocity distribution.
    It is represented in terms of a B-spline interpolator of degree N for each of spatial apertures
    (regions on the image plane delineated by arbitrary polygons).
    For each aperture we thus have a vector of B-spline amplitudes.
    The LOSVD is constructed in two steps:
    - first the raw data for an orbit is stored in an external array,
    by adding each point on the trajectory, weighted by the time spent at this point.
    The 6d phase-space point is first converted to a 2d point in the image plane and the velocity
    perpendicular to this plane, using the rotation matrix provided to the constructor.
    Then the contribution of this point to each of the basis functions in the internally managed
    3d tensor-product B-spline interpolator is computed. The maximum number of basis functions affected
    by a single point is (N+1)^3. This contribution, weighted with the provided multiplicative factor,
    is stored in an external 2d matrix.
    - at the end of orbit integration, this datacube is converted to a 2d matrix of amplitudes,
    where each row represents the data for a single aperture.
    The last step uses two auxiliary matrices for the spatial and velocity directions, correspondingly,
    which are initialized in the constructor.
    \tparam N  is the degree of B-spline interpolators (0,1,2 or 3).
    Higher-degree interpolators are more accurate and allow a larger pixel size
    (fewer expansion coefficients).
*/
template<int N>
class TargetLOSVD: public BaseTarget {
    double transformMatrix[9];  ///< rotation matrix for transforming intrinsic to projected coords
    const math::BsplineInterpolator1d<N> bsplx, bsply, bsplv;  ///< basis-set interpolators
    math::Matrix<double> apertureConvolutionMatrix;  ///< spatial convolution and rebinning matrix
    math::Matrix<double> velocityConvolutionMatrix;  ///< velocity convolution matrix
    const coord::SymmetryType symmetry;  ///< symmetry of the potential and the orbital shape
    bool symmetricGrids;                 ///< whether the input grids are reflection-symmetric
public:
    /// construct the grid with given parameters
    /// \throw std::invalid_argument if the parameters are incorrect
    TargetLOSVD(const LOSVDParams& params);

    virtual const char* name() const;
    virtual std::string coefName(unsigned int index) const;

    /// return the total number of points in the flattened datacube
    virtual unsigned int numValues() const {
        return bsplx.numValues() * bsply.numValues() * bsplv.numValues();
    }

    /// return the number of coefficients in the output array:
    /// the number of apertures times the number of amplitudes of B-spline expansion of LOSVD
    virtual unsigned int numCoefs() const {
        return apertureConvolutionMatrix.rows() * bsplv.numValues();
    }

    /// allocate a new internal 3d data cube stored in a 2d matrix of the appropriate shape
    virtual math::Matrix<double> newDatacube() const {
        return math::Matrix<double>(bsplx.numValues() * bsply.numValues(), bsplv.numValues(), 0.);
    }

    /// add a weighted point to the datacube.
    /// \param[in]  point  is the 6d point in phase space: x,y,z,vx,vy,vz, which is
    /// internally converted to the sky plane position x', y' and the line-of-sight velocity v_los;
    /// \param[in]  mult  is the weight of input point;
    /// \param[in,out]  datacube points to the external array storing the flattened 3d data cube;
    /// all its elements that have a contribution from the input point are incremented by
    /// the weights of corresponding basis functions multiplied by the input factor 'mult'.
    virtual void addPoint(const double point[6], const double mult, double* datacube) const;

    /// convert the intermediate data stored in the regular 3d data cube
    /// into the array of basis function amplitudes for the LOSVD in each aperture
    virtual void finalizeDatacube(math::Matrix<double> &datacube, StorageNumT* output) const;

    /// compute the normalizations of the LOSVD (total mass in each aperture, i.e., integral of
    /// surface density over the aperture, convolved with the spatial PSF)
    virtual std::vector<double> computeDensityProjection(const potential::BaseDensity& density) const;
};


/// A simple class for recording radial and tangential velocity dispersions in spherical shells
template<int N>
class TargetKinemShell: public BaseTarget {
    const math::BsplineInterpolator1d<N> bspl;  ///< B-spline for representing rho * sigma^2
public:
    /// construct the target from the provided grid in spherical radius (should start at r=0)
    TargetKinemShell(const std::vector<double>& gridr) : bspl(gridr) {}
    virtual const char* name() const;
    virtual std::string coefName(unsigned int index) const;
    virtual void addPoint(const double point[6], double mult, double output[]) const;
    virtual unsigned int numVars() const { return 6; }
    virtual unsigned int numValues() const { return bspl.numValues() * 2; }
    /// this does not make sense for this target - throws a std::runtime_error
    virtual std::vector<double> computeDensityProjection(const potential::BaseDensity&) const;
};

}  // namespace
