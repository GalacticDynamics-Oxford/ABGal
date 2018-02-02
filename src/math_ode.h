/** \file    math_ode.h
    \brief   ODE integration classes
    \author  Eugene Vasiliev
    \date    2008-2018

    This module implements classes for integration of ordinary differential equation systems.

    OdeSolverDOP853 is a modification of the 8th order Runge-Kutta Solver from
    Hairer, Norsett & Wanner, "Solving ordinary differential equations", 1987, Berlin:Springer.
    Based on the C version (by J.Colinge) of the original Fortran code by E.Hairer & G.Wanner.
*/

#pragma once
#include <vector>

namespace math{

/** Prototype of a function that is used in integration of ordinary differential equation systems:
    dx/dt = f(t, x), where y is an N-dimensional vector. */
class IOdeSystem {
public:
    IOdeSystem() {};
    virtual ~IOdeSystem() {};

    /** Compute the r.h.s. of the differential equation: 
        \param[in]  t    is the integration variable (time),
        \param[in]  x    is the vector of values of dependent variables, 
        \param[out] dxdt should return the time derivatives of these variables */
    virtual void eval(const double t, const double x[], double dxdt[]) const = 0;

    /** Return the size of ODE system (number of variables N) */
    virtual unsigned int size() const = 0;
};

/** Prototype of a function that is used in integration of second-order
    linear ordinary differential equation systems with variable coefficients:
    d2x(t) / dt2 = c(t) x(t), where x is an N-dimensional vector and c is a N by N matrix. */
class IOde2System {
public:
    IOde2System() {};
    virtual ~IOde2System() {};
    
    /** Compute the matrix c in the r.h.s. of the differential equation: 
        \param[in]  t    is the integration variable (time),
        \param[out] mat  should point to an existing array of length N^2,
        which will be filled with the flattened (row-major) matrix c: mat[i*N+j] = c_{ij}
    */
    virtual void eval(double t, double mat[]) const = 0;
    
    /** Return the size of ODE system (2N variables - vectors x and dx/dt) */
    virtual unsigned int size() const = 0;
};


/** basic class for numerical integrators of ODE systems */
class BaseOdeSolver {
public:
    BaseOdeSolver(const IOdeSystem& _odeSystem):
        odeSystem(_odeSystem), time(0) {};

    virtual ~BaseOdeSolver() {};

    /** (re-)initialize the internal state from the given ODE system state */
    virtual void init(const double stateNew[]) = 0;

    /** advance the solution by one timestep.
        \param[in]  dt is the length of the timestep;
        if 0 then it will be determined automatically by internal accuracy requirements;
        \return the length of the timestep taken, or zero on error
    */
    virtual double doStep(double dt = 0) = 0;

    /** report the number of variables in the ODE system */
    inline unsigned int size() const { return odeSystem.size(); }

    /** return the time to which the integration has proceeded so far */
    inline double getTime() const { return time; }

    /** return the interpolated solution
        \param[in]  t  is the moment of time, which should lie within the last completed timestep;
        \param[in]  ind  is the index of the component of the solution vector;
        \return  the interpolated solution at the given time.
        \throw  std::out_of_range if the index is not in the range (0 .. N-1)
    */
    virtual double getSol(double t, unsigned int ind) const = 0;

protected:
    /// the object providing the r.h.s. of the ODE
    const IOdeSystem& odeSystem;
    /// current value of integration variable (time), incremented after each timestep
    double time;
};


/** 8th order Runge-Kutta integrator with 7th order interpolation for the dense output
    (modification of the original algorithm from Hairer,Norsett&Wanner, reducing the order of
    interpolation from 8 to 7 and saving 3 function evaluations per timestep) */
class OdeSolverDOP853: public BaseOdeSolver {
public:
    OdeSolverDOP853(const IOdeSystem& _odeSystem, double _accRel=1e-8, double _accAbs=0) :
        BaseOdeSolver(_odeSystem), NDIM(odeSystem.size()),
        accRel(_accRel), accAbs(_accAbs),
        timePrev(0), nextTimeStep(0),
        state(NDIM * 10)  // storage for the current values and derivs of x and for 8 interpolation coefs
    {}    
    virtual void init(const double stateNew[]);
    virtual double doStep(double dt = 0);
    virtual double getSol(double t, unsigned int ind) const;
    /// return the estimate for the length of the next timestep
    /// (the actual timestep may happen to be shorter, if the error is unacceptably large)
    inline double getTimeStep() const { return nextTimeStep; }
private:
    const int NDIM;              ///< number of equations
    const double accRel, accAbs; ///< relative and absolute tolerance parameters
    double timePrev;             ///< value of time at the beginning of the completed timestep
    double nextTimeStep;         ///< length of next timestep (not the one just completed)
    std::vector<double> state;   ///< 10*NDIM values: x, dx/dt, and 8 interpolation coefs for dense output

    double initTimeStep();       ///< determine the initial timestep
};


/** basic class for numerical integrators of second-order linear ODE systems:
    x''(t) = C(t) x(t),  where x is a N-dimensional vector and C is a NxN matrix.
    It is intended for solving the variational equation during orbit integration.
*/
class BaseOde2Solver {
public:
    BaseOde2Solver(const IOde2System& _odeSystem):
        odeSystem(_odeSystem), time(0) {};

    virtual ~BaseOde2Solver() {};

    /** (re-)initialize the internal state from the given ODE system state */
    virtual void init(const double stateNew[]) = 0;

    /** advance the solution by one timestep of length dt */
    virtual void doStep(double dt) = 0;

    /** report the number of variables in the ODE system (both x and dx/dt, i.e. 2N) */
    inline unsigned int size() const { return odeSystem.size(); }

    /** return the interpolated solution
        \param[in]  t  is the moment of time, which must lie within current timestep interval;
        \param[in]  ind  is the index of the component of the solution vector:
        0 <= ind < N corresponds to x, N <= ind < 2N - to dx/dt;
        \return  the interpolated solution at the given time.
        \throw  std::out_of_range if the index is not in the range (0 .. 2N-1)
    */
    virtual double getSol(double t, unsigned int ind) const = 0;

protected:
    /// object providing the r.h.s. of the ODE
    const IOde2System& odeSystem;
    /// current value of integration variable (time), incremented after each timestep
    double time;
};


/** Implicit method with 3 Gauss-Legendre collocation points;
    the order of solution is 6, the order of interpolation is 5 for x, 4 for dx/dt.
    \tparam NDIM is the size of vector x (hence the size of the entire ODE system is 2 NDIM);
    only the cases NDIM=1,2,3 are compiled.
*/
template<int NDIM>
class Ode2SolverGL3: public BaseOde2Solver {
public:
    Ode2SolverGL3(const IOde2System& _odeSystem);
    virtual void init(const double stateNew[]);
    virtual void doStep(double dt);
    virtual double getSol(double t, unsigned int ind) const;

private:
    double state[NDIM*2], p[NDIM], q[NDIM], r[NDIM];
};

/** Implicit method with 4 Gauss-Legendre collocation points;
    the order of solution is 8, the order of interpolation is 6 for x, 5 for dx/dt */
template<int NDIM>
class Ode2SolverGL4: public BaseOde2Solver {
public:
    Ode2SolverGL4(const IOde2System& _odeSystem);
    virtual void init(const double stateNew[]);
    virtual void doStep(double dt);
    virtual double getSol(double t, unsigned int ind) const;

private:
    double state[NDIM*2], p[NDIM], q[NDIM], r[NDIM], s[NDIM];
};

};  // namespace
