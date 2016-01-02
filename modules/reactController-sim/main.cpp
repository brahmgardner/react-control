/*
 * Copyright (C) 2015 iCub Facility - Istituto Italiano di Tecnologia
 * Author: Ugo Pattacini
 * email:  ugo.pattacini@iit.it
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <csignal>
#include <cmath>
#include <limits>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <deque>

#include <IpTNLP.hpp>
#include <IpIpoptApplication.hpp>

#include <yarp/os/all.h>
#include <yarp/sig/all.h>
#include <yarp/math/Math.h>

#include <iCub/ctrl/math.h>
#include <iCub/ctrl/pids.h>
#include <iCub/ctrl/minJerkCtrl.h>
#include <iCub/iKin/iKinFwd.h>

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;
using namespace iCub::ctrl;
using namespace iCub::iKin;


/****************************************************************/
class ControllerNLP : public Ipopt::TNLP
{
protected:
    iKinChain &chain;

    Vector xr;
    Vector x0;
    Vector delta_x;
    Vector v0;
    Matrix v_lim;
    Matrix J0;
    Vector v;
    double dt;

    Vector qGuard;
    Vector qGuardMinExt;
    Vector qGuardMinInt;
    Vector qGuardMinCOG;
    Vector qGuardMaxExt;
    Vector qGuardMaxInt;
    Vector qGuardMaxCOG;

    /****************************************************************/
    void computeGuard()
    {
        double guardRatio=0.1;
        qGuard.resize(chain.getDOF());
        qGuardMinExt.resize(chain.getDOF());
        qGuardMinInt.resize(chain.getDOF());
        qGuardMinCOG.resize(chain.getDOF());
        qGuardMaxExt.resize(chain.getDOF());
        qGuardMaxInt.resize(chain.getDOF());
        qGuardMaxCOG.resize(chain.getDOF());

        for (size_t i=0; i<chain.getDOF(); i++)
        {
            qGuard[i]=0.25*guardRatio*(chain(i).getMax()-chain(i).getMin());

            qGuardMinExt[i]=chain(i).getMin()+qGuard[i];
            qGuardMinInt[i]=qGuardMinExt[i]+qGuard[i];
            qGuardMinCOG[i]=0.5*(qGuardMinExt[i]+qGuardMinInt[i]);

            qGuardMaxExt[i]=chain(i).getMax()-qGuard[i];
            qGuardMaxInt[i]=qGuardMaxExt[i]-qGuard[i];
            qGuardMaxCOG[i]=0.5*(qGuardMaxExt[i]+qGuardMaxInt[i]);
        }
    }

    /****************************************************************/
    Matrix computeWeight()
    {
        Matrix w(chain.getDOF(),2);
        for (size_t i=0; i<chain.getDOF(); i++)
        {
            double qi=chain(i).getAng();
            if ((qi>=qGuardMinInt[i]) && (qi<=qGuardMaxInt[i]))
                w(i,0)=w(i,1)=1.0;
            else if ((qi<=qGuardMinExt[i]) || (qi>=qGuardMaxExt[i]))
                w(i,i)=0.0;
            else if (qi<qGuardMinInt[i])
            {
                w(i,0)=0.5*(1.0+tanh(+10.0*(qi-qGuardMinCOG[i])/qGuard[i]));
                w(i,1)=1.0;
            }
            else
            {
                w(i,0)=1.0;
                w(i,1)=0.5*(1.0+tanh(-10.0*(qi-qGuardMaxCOG[i])/qGuard[i]));
            }
        }
        return w;
    }

public:
    /****************************************************************/
    ControllerNLP(iKinChain &chain_) : chain(chain_)
    {
        xr.resize(3,0.0);
        delta_x.resize(3,0.0);
        v0.resize(chain.getDOF(),0.0);
        v=v0;

        v_lim.resize(chain.getDOF(),2);
        for (size_t r=0; r<chain.getDOF(); r++)
        {
            v_lim(r,1)=std::numeric_limits<double>::max();
            v_lim(r,0)=-v_lim(r,1);
        }

        computeGuard();
        dt=0.0;
    }

    /****************************************************************/
    void set_xr(const Vector &xr)
    {
        this->xr=xr;
    }

    /****************************************************************/
    void set_v_lim(const Matrix &v_lim)
    {
        this->v_lim=CTRL_DEG2RAD*v_lim;
    }

    /****************************************************************/
    void set_dt(const double dt)
    {
        this->dt=dt;
    }

    /****************************************************************/
    void set_v0(const Vector &v0)
    {
        this->v0=CTRL_DEG2RAD*v0;
    }

    /****************************************************************/
    Vector get_result() const
    {
        return v;
    }

    /****************************************************************/
    bool get_nlp_info(Ipopt::Index &n, Ipopt::Index &m, Ipopt::Index &nnz_jac_g,
                      Ipopt::Index &nnz_h_lag, IndexStyleEnum &index_style)
    {
        n=chain.getDOF();
        m=nnz_jac_g=nnz_h_lag=0;
        index_style=TNLP::C_STYLE;

        x0=chain.EndEffPosition();
        J0=chain.GeoJacobian().submatrix(0,2,0,chain.getDOF()-1);
        return true;
    }

    /****************************************************************/
    bool get_bounds_info(Ipopt::Index n, Ipopt::Number *x_l, Ipopt::Number *x_u,
                         Ipopt::Index m, Ipopt::Number *g_l, Ipopt::Number *g_u)
    {
        Matrix w=computeWeight();
        for (Ipopt::Index i=0; i<n; i++)
        {
            x_l[i]=w(i,0)*v_lim(i,0);
            x_u[i]=w(i,1)*v_lim(i,1);
        }
        return true;
    }

    /****************************************************************/
    bool get_starting_point(Ipopt::Index n, bool init_x, Ipopt::Number *x,
                            bool init_z, Ipopt::Number *z_L, Ipopt::Number *z_U,
                            Ipopt::Index m, bool init_lambda, Ipopt::Number *lambda)
    {
        for (Ipopt::Index i=0; i<n; i++)
            x[i]=v0[i];
        return true;
    }

    /************************************************************************/
    void computeQuantities(const Ipopt::Number *x, const bool new_x)
    {
        if (new_x)
        {
            for (size_t i=0; i<v.length(); i++)
                v[i]=x[i];
            delta_x=xr-(x0+dt*(J0*v));
        }
    }

    /****************************************************************/
    bool eval_f(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                Ipopt::Number &obj_value)
    {
        computeQuantities(x,new_x);
        obj_value=norm2(delta_x);
        return true;
    }

    /****************************************************************/
    bool eval_grad_f(Ipopt::Index n, const Ipopt::Number* x, bool new_x,
                     Ipopt::Number *grad_f)
    {
        computeQuantities(x,new_x);
        for (Ipopt::Index i=0; i<n; i++)
            grad_f[i]=-2.0*dt*dot(delta_x,J0.getCol(i));
        return true; 
    }

    /****************************************************************/
    bool eval_g(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                Ipopt::Index m, Ipopt::Number *g)
    {
        return true;
    }

    /****************************************************************/
    bool eval_jac_g(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                    Ipopt::Index m, Ipopt::Index nele_jac, Ipopt::Index *iRow,
                    Ipopt::Index *jCol, Ipopt::Number *values)
    {
        return true;
    }

    /****************************************************************/
    bool eval_h(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                Ipopt::Number obj_factor, Ipopt::Index m, const Ipopt::Number *lambda,
                bool new_lambda, Ipopt::Index nele_hess, Ipopt::Index *iRow,
                Ipopt::Index *jCol, Ipopt::Number *values)
    {
        return true;
    }

    /****************************************************************/
    void finalize_solution(Ipopt::SolverReturn status, Ipopt::Index n,
                           const Ipopt::Number *x, const Ipopt::Number *z_L,
                           const Ipopt::Number *z_U, Ipopt::Index m,
                           const Ipopt::Number *g, const Ipopt::Number *lambda,
                           Ipopt::Number obj_value, const Ipopt::IpoptData *ip_data,
                           Ipopt::IpoptCalculatedQuantities *ip_cq)
    {
        for (Ipopt::Index i=0; i<n; i++)
            v[i]=CTRL_RAD2DEG*x[i];
    }
};


/****************************************************************/
class AvoidanceHandler
{
protected:
    iKinChain &chain;
    deque<iKinChain*> chainCtrlPoints;

public:
    /****************************************************************/
    AvoidanceHandler(iKinChain &chain_) : chain(chain_)
    {
        iCubArm *arm;
        Matrix HN=eye(4,4);

        arm=new iCubArm("left");
        iKinChain *c1=arm->asChain();
        c1->releaseLink(0);
        c1->releaseLink(1);
        c1->releaseLink(2);
        c1->rmLink(9);
        c1->rmLink(8);
        c1->rmLink(7);
        HN(2,3)=0.1373;
        c1->setHN(HN);

        arm=new iCubArm("left");
        iKinChain *c2=arm->asChain();
        c2->releaseLink(0);
        c2->releaseLink(1);
        c2->releaseLink(2);
        c2->rmLink(9);
        c2->rmLink(8);
        c2->rmLink(7);
        HN(2,3)=0.1373/2.0;
        c2->setHN(HN);

        chainCtrlPoints.push_back(c1);
        chainCtrlPoints.push_back(c2);
    }

    /****************************************************************/
    ~AvoidanceHandler()
    {
        for (size_t i=0; i<chainCtrlPoints.size(); i++)
            delete chainCtrlPoints[i];
    }

    /****************************************************************/
    deque<Vector> getCtrlPointsPosition()
    {
        deque<Vector> ctrlPoints;
        for (size_t i=0; i<chainCtrlPoints.size(); i++)
        {
            for (size_t j=0; j<chainCtrlPoints[i]->getDOF(); j++)
                chainCtrlPoints[i]->setAng(j,chain(j).getAng());
            ctrlPoints.push_back(chainCtrlPoints[i]->EndEffPosition());
        }
        
        return ctrlPoints;
    }
};


/****************************************************************/
namespace
{
    volatile std::sig_atomic_t gSignalStatus;
}


/****************************************************************/
void signal_handler(int signal)
{
    gSignalStatus=signal;
}


/****************************************************************/
int main()
{
    iCubArm arm("left");
    iKinChain &chain=*arm.asChain();
    chain.releaseLink(0);
    chain.releaseLink(1);
    chain.releaseLink(2);

    Vector q0(chain.getDOF(),0.0);
    q0[3]=-25.0; q0[4]=20.0; q0[6]=50.0;
    chain.setAng(CTRL_DEG2RAD*q0);

    Matrix lim(chain.getDOF(),2);
    Matrix v_lim(chain.getDOF(),2);
    for (size_t r=0; r<chain.getDOF(); r++)
    {
        lim(r,0)=CTRL_RAD2DEG*chain(r).getMin();
        lim(r,1)=CTRL_RAD2DEG*chain(r).getMax();
        v_lim(r,0)=-50.0;
        v_lim(r,1)=+50.0;
    }
    v_lim(1,0)=v_lim(1,1)=0.0; // don't use torso roll

    Ipopt::SmartPtr<Ipopt::IpoptApplication> app=new Ipopt::IpoptApplication;
    app->Options()->SetNumericValue("tol",1e-6);
    app->Options()->SetNumericValue("constr_viol_tol",1e-8);
    app->Options()->SetIntegerValue("acceptable_iter",0);
    app->Options()->SetStringValue("mu_strategy","adaptive");
    app->Options()->SetIntegerValue("max_iter",10000);
    app->Options()->SetNumericValue("max_cpu_time",10.0);
    app->Options()->SetStringValue("nlp_scaling_method","gradient-based");
    app->Options()->SetNumericValue("nlp_scaling_max_gradient",1.0);
    app->Options()->SetNumericValue("nlp_scaling_min_value",1e-6);
    app->Options()->SetStringValue("hessian_approximation","limited-memory");
    app->Options()->SetStringValue("derivative_test","none");
    //app->Options()->SetStringValue("derivative_test","first-order");
    app->Options()->SetIntegerValue("print_level",0);
    app->Initialize();

    Ipopt::SmartPtr<ControllerNLP> nlp=new ControllerNLP(chain);
    AvoidanceHandler avhandler(chain);

    double dt=0.01;
    double T=1.0;

    nlp->set_dt(dt);
    Integrator motors(dt,q0,lim);
    Vector v(chain.getDOF(),0.0);

    Vector xee=chain.EndEffPosition();
    minJerkTrajGen target(xee,dt,T);
    Vector xc(3);
    xc[0]=-0.35;
    xc[1]=0.0;
    xc[2]=0.1;
    double rt=0.1;

    Vector xo(3);
    xo[0]=-0.3;
    xo[1]=0.0;
    xo[2]=0.4;
    double ro=0.04;
    Vector vo(3,0.0);
    vo[2]=-0.05;
    Integrator obstacle(dt,xo);

    ofstream fout;
    fout.open("data.log");

    std::signal(SIGINT,signal_handler);
    for (double t=0.0; t<10.0; t+=dt)
    {
        Vector xd=xc;
        xd[1]+=rt*cos(2.0*M_PI*0.3*t);
        xd[2]+=rt*sin(2.0*M_PI*0.3*t);

        target.computeNextValues(xd);
        Vector xr=target.getPos();

        xo=obstacle.integrate(vo);

        nlp->set_xr(xr);
        nlp->set_v_lim(v_lim);
        nlp->set_v0(v);

        Ipopt::ApplicationReturnStatus status=app->OptimizeTNLP(GetRawPtr(nlp));

        v=nlp->get_result();
        xee=chain.EndEffPosition(CTRL_DEG2RAD*motors.integrate(v));

        yInfo()<<"        t [s] = "<<t;
        yInfo()<<"    v [deg/s] = ("<<v.toString(3,3).c_str()<<")";
        yInfo()<<" |xr-xee| [m] = "<<norm(xr-xee);
        yInfo()<<"";
        
        ostringstream strCtrlPoints;
        deque<Vector> ctrlPoints=avhandler.getCtrlPointsPosition();
        for (size_t i=0; i<ctrlPoints.size(); i++)
            strCtrlPoints<<ctrlPoints[i].toString(3,3).c_str()<<" ";

        fout<<t<<" "<<
              xr.toString(3,3).c_str()<<" "<<
              xo.toString(3,3).c_str()<<" "<<
              ro<<" "<<
              v.toString(3,3).c_str()<<" "<<
              (CTRL_RAD2DEG*chain.getAng()).toString(3,3).c_str()<<" "<<
              strCtrlPoints.str()<<
              endl;

        if (gSignalStatus==SIGINT)
        {
            yWarning("SIGINT detected: exiting ...");
            break;
        }
    }

    fout.close();
    return 0;
}
