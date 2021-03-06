/* 
 * Copyright: (C) 2015 iCub Facility - Istituto Italiano di Tecnologia
 * Author: Ugo Pattacini, Matej Hoffmann, Alessandro Roncone; <name.surname@iit.it>
 * website: www.robotcub.org
 * 
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
 * Public License for more details.
*/

#include "reactIpOpt.h";


    /****************************************************************/
    void ControllerNLP::computeSelfAvoidanceConstraints()
    {
        double joint1_0, joint1_1;
        double joint2_0, joint2_1;
        joint1_0= 28.0*CTRL_DEG2RAD;
        joint1_1= 23.0*CTRL_DEG2RAD;
        joint2_0=-37.0*CTRL_DEG2RAD;
        joint2_1= 80.0*CTRL_DEG2RAD;
        shou_m=(joint1_1-joint1_0)/(joint2_1-joint2_0);
        shou_n=joint1_0-shou_m*joint2_0;

        double joint3_0, joint3_1;
        double joint4_0, joint4_1;
        joint3_0= 85.0*CTRL_DEG2RAD;
        joint3_1=105.0*CTRL_DEG2RAD;
        joint4_0= 90.0*CTRL_DEG2RAD;
        joint4_1= 40.0*CTRL_DEG2RAD;
        elb_m=(joint4_1-joint4_0)/(joint3_1-joint3_0);
        elb_n=joint4_0-elb_m*joint3_0;
    }

    /****************************************************************/
    void ControllerNLP::computeGuard()
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
    void ControllerNLP::computeBounds()
    {
        for (size_t i=0; i<chain.getDOF(); i++)
        {
            double qi=q0[i];
            if ((qi>=qGuardMinInt[i]) && (qi<=qGuardMaxInt[i]))
                bounds(i,0)=bounds(i,1)=1.0;
            else if (qi<qGuardMinInt[i])
            {
                bounds(i,0)=(qi<=qGuardMinExt[i]?0.0:
                             0.5*(1.0+tanh(+10.0*(qi-qGuardMinCOG[i])/qGuard[i]))); 
                bounds(i,1)=1.0;
            }
            else
            {
                bounds(i,0)=1.0;
                bounds(i,1)=(qi>=qGuardMaxExt[i]?0.0:
                             0.5*(1.0+tanh(-10.0*(qi-qGuardMaxCOG[i])/qGuard[i])));
            }
        }

        for (size_t i=0; i<chain.getDOF(); i++)
        {
            bounds(i,0)*=v_lim(i,0);
            bounds(i,1)*=v_lim(i,1);
        }
    }

    /****************************************************************/
    Matrix ControllerNLP::v2m(const Vector &x)
    {
        yAssert(x.length()>=6);
        Vector ang=x.subVector(3,5);
        double ang_mag=norm(ang);
        if (ang_mag>0.0)
            ang/=ang_mag;
        ang.push_back(ang_mag);
        Matrix H=axis2dcm(ang);
        H(0,3)=x[0];
        H(1,3)=x[1];
        H(2,3)=x[2];
        return H;
    }

    /****************************************************************/
    Matrix ControllerNLP::skew(const Vector &w)
    {
        yAssert(w.length()>=3);
        Matrix S(3,3);
        S(0,0)=S(1,1)=S(2,2)=0.0;
        S(1,0)= w[2]; S(0,1)=-S(1,0);
        S(2,0)=-w[1]; S(0,2)=-S(2,0);
        S(2,1)= w[0]; S(1,2)=-S(2,1);
        return S;
    }

//public:
    /****************************************************************/
    ControllerNLP::ControllerNLP(iKinChain &chain_) : chain(chain_)
    {
        xr.resize(6,0.0);
        set_xr(xr);

        v0.resize(chain.getDOF(),0.0); v=v0;
        He=zeros(4,4); He(3,3)=1.0;

        q_lim.resize(chain.getDOF(),2);
        v_lim.resize(chain.getDOF(),2);        
        for (size_t r=0; r<chain.getDOF(); r++)
        {
            q_lim(r,0)=chain(r).getMin();
            q_lim(r,1)=chain(r).getMax();

            v_lim(r,1)=std::numeric_limits<double>::max();
            v_lim(r,0)=-v_lim(r,1);
        }
        bounds=v_lim;
        
        computeSelfAvoidanceConstraints();
        computeGuard();

        hitting_constraints=true;
        orientation_control=true;
        dt=0.01;
    }

    /****************************************************************/
    void ControllerNLP::set_xr(const Vector &xr)
    {
        yAssert(this->xr.length()==xr.length());
        this->xr=xr;

        Hr=v2m(xr);
        pr=xr.subVector(0,2);

        skew_nr=skew(Hr.getCol(0));
        skew_sr=skew(Hr.getCol(1));
        skew_ar=skew(Hr.getCol(2));
    }

    /****************************************************************/
    void ControllerNLP::set_v_limInDegPerSecond(const Matrix &v_lim)
    {
        yAssert((this->v_lim.rows()==v_lim.rows()) &&
                (this->v_lim.cols()==v_lim.cols()));

        for (int r=0; r<v_lim.rows(); r++)
            yAssert(v_lim(r,0)<=v_lim(r,1));

        this->v_lim=CTRL_DEG2RAD*v_lim;
    }

    /****************************************************************/
    void ControllerNLP::set_hitting_constraints(const bool _hitting_constraints)
    {
        hitting_constraints=_hitting_constraints;
    }

    /****************************************************************/
    void ControllerNLP::set_orientation_control(const bool _orientation_control)
    {
        orientation_control=_orientation_control;
    }

    /****************************************************************/
    void ControllerNLP::set_dt(const double dt)
    {
        yAssert(dt>0.0);
        this->dt=dt;
    }

    /****************************************************************/
    void ControllerNLP::set_v0InDegPerSecond(const Vector &v0)
    {
        yAssert(this->v0.length()==v0.length());
        this->v0=CTRL_DEG2RAD*v0;
    }

    /****************************************************************/
    void ControllerNLP::init()
    {
        q0=chain.getAng();
        H0=chain.getH();
        R0=H0.submatrix(0,2,0,2);
        p0=H0.getCol(3).subVector(0,2);

        Matrix J0=chain.GeoJacobian();
        J0_xyz=J0.submatrix(0,2,0,chain.getDOF()-1);
        J0_ang=J0.submatrix(3,5,0,chain.getDOF()-1);

        computeBounds();
    }

    /****************************************************************/
    Vector ControllerNLP::get_resultInDegPerSecond() const
    {
        return CTRL_RAD2DEG*v;
    }

    /****************************************************************/
    Property ControllerNLP::getParameters() const
    {
        Property parameters;
        parameters.put("dt",dt);
        return parameters;
    }

    /****************************************************************/
    bool ControllerNLP::get_nlp_info(Ipopt::Index &n, Ipopt::Index &m, Ipopt::Index &nnz_jac_g,
                      Ipopt::Index &nnz_h_lag, IndexStyleEnum &index_style)
    {
        n=chain.getDOF();

        // reaching in position
        m=1; nnz_jac_g=n;

        if (hitting_constraints)
        {
            // shoulder's cables length
            m+=3; nnz_jac_g+=2+3+2;

            // avoid hitting torso
            m+=1; nnz_jac_g+=2;

            // avoid hitting forearm
            m+=2; nnz_jac_g+=2+2;
        }

        nnz_h_lag=0;
        index_style=TNLP::C_STYLE;
        return true;
    }

    /****************************************************************/
    bool ControllerNLP::get_bounds_info(Ipopt::Index n, Ipopt::Number *x_l, Ipopt::Number *x_u,
                         Ipopt::Index m, Ipopt::Number *g_l, Ipopt::Number *g_u)
    {
        for (Ipopt::Index i=0; i<n; i++)
        {
            x_l[i]=bounds(i,0);
            x_u[i]=bounds(i,1);            
        }

        // reaching in position
        g_l[0]=g_u[0]=0.0;

        if (hitting_constraints)
        {
            // shoulder's cables length
            g_l[1]=-347.00*CTRL_DEG2RAD;
            g_u[1]=std::numeric_limits<double>::max();
            g_l[2]=-366.57*CTRL_DEG2RAD;
            g_u[2]=112.42*CTRL_DEG2RAD;
            g_l[3]=-66.60*CTRL_DEG2RAD;
            g_u[3]=213.30*CTRL_DEG2RAD;

            // avoid hitting torso
            g_l[4]=shou_n;
            g_u[4]=std::numeric_limits<double>::max();

            // avoid hitting forearm
            g_l[5]=-std::numeric_limits<double>::max();
            g_u[5]=elb_n;
            g_l[6]=-elb_n;
            g_u[6]=std::numeric_limits<double>::max();
        }

        return true;
    }

    /****************************************************************/
    bool ControllerNLP::get_starting_point(Ipopt::Index n, bool init_x, Ipopt::Number *x,
                            bool init_z, Ipopt::Number *z_L, Ipopt::Number *z_U,
                            Ipopt::Index m, bool init_lambda, Ipopt::Number *lambda)
    {
        for (Ipopt::Index i=0; i<n; i++)
            x[i]=std::min(std::max(bounds(i,0),v0[i]),bounds(i,1));
        return true;
    }

    /************************************************************************/
    void ControllerNLP::computeQuantities(const Ipopt::Number *x, const bool new_x)
    {
        if (new_x)
        {
            for (size_t i=0; i<v.length(); i++)
                v[i]=x[i];

            He.setSubmatrix(R0+dt*(skew(J0_ang*v)*R0),0,0);
            Vector pe=p0+dt*(J0_xyz*v);            
            He(0,3)=pe[0];
            He(1,3)=pe[1];
            He(2,3)=pe[2];

            err_xyz=pr-pe;
            err_ang=dcm2axis(Hr*He.transposed());
            err_ang*=err_ang[3];
            err_ang.pop_back();

            Matrix L=-0.5*(skew_nr*skew(He.getCol(0))+
                           skew_sr*skew(He.getCol(1))+
                           skew_ar*skew(He.getCol(2)));
            Derr_ang=-dt*(L*J0_ang);
        }
    }

    /****************************************************************/
    bool ControllerNLP::eval_f(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                Ipopt::Number &obj_value)
    {
        computeQuantities(x,new_x);
        obj_value=(orientation_control?norm2(err_ang):0.0);
        return true;
    }

    /****************************************************************/
    bool ControllerNLP::eval_grad_f(Ipopt::Index n, const Ipopt::Number* x, bool new_x,
                     Ipopt::Number *grad_f)
    {
        computeQuantities(x,new_x);
        for (Ipopt::Index i=0; i<n; i++)
            grad_f[i]=(orientation_control?2.0*dot(err_ang,Derr_ang.getCol(i)):0.0);
        return true; 
    }

    /****************************************************************/
    bool ControllerNLP::eval_g(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                Ipopt::Index m, Ipopt::Number *g)
    {
        computeQuantities(x,new_x);

        // reaching in position
        g[0]=norm2(err_xyz);

        if (hitting_constraints)
        {
            // shoulder's cables length
            g[1]=1.71*(q0[3+0]+dt*x[3+0]-(q0[3+1]+dt*x[3+1]));
            g[2]=1.71*(q0[3+0]+dt*x[3+0]-(q0[3+1]+dt*x[3+1])-(q0[3+2]+dt*x[3+2]));
            g[3]=q0[3+1]+dt*x[3+1]+q0[3+2]+dt*x[3+2];

            // avoid hitting torso
            g[4]=q0[3+1]+dt*x[3+1]-shou_m*(q0[3+2]+dt*x[3+2]);

            // avoid hitting forearm
            g[5]=-elb_m*(q0[3+3+0]+dt*x[3+3+0])+q0[3+3+1]+dt*x[3+3+1];
            g[6]=elb_m*(q0[3+3+0]+dt*x[3+3+0])+q0[3+3+1]+dt*x[3+3+1];
        }

        return true;
    }

    /****************************************************************/
    bool ControllerNLP::eval_jac_g(Ipopt::Index n, const Ipopt::Number *x, bool new_x,
                    Ipopt::Index m, Ipopt::Index nele_jac, Ipopt::Index *iRow,
                    Ipopt::Index *jCol, Ipopt::Number *values)
    {
        if (values==NULL)
        {
            Ipopt::Index idx=0;

            // reaching in position
            for (Ipopt::Index i=0; i<n; i++)
            {
                iRow[i]=0; jCol[i]=i;
                idx++;
            }

            if (hitting_constraints)
            {
                // shoulder's cables length
                iRow[idx]=1; jCol[idx]=3+0; idx++;
                iRow[idx]=1; jCol[idx]=3+1; idx++;

                iRow[idx]=2; jCol[idx]=3+0; idx++;
                iRow[idx]=2; jCol[idx]=3+1; idx++;
                iRow[idx]=2; jCol[idx]=3+2; idx++;

                iRow[idx]=3; jCol[idx]=3+1; idx++;
                iRow[idx]=3; jCol[idx]=3+2; idx++;

                // avoid hitting torso
                iRow[idx]=4; jCol[idx]=3+1; idx++;
                iRow[idx]=4; jCol[idx]=3+2; idx++;

                // avoid hitting forearm
                iRow[idx]=5; jCol[idx]=3+3+0; idx++;
                iRow[idx]=5; jCol[idx]=3+3+1; idx++;

                iRow[idx]=6; jCol[idx]=3+3+0; idx++;
                iRow[idx]=6; jCol[idx]=3+3+1; idx++;
            }
        }
        else
        {
            computeQuantities(x,new_x);

            Ipopt::Index idx=0;

            // reaching in position
            for (Ipopt::Index i=0; i<n; i++)
            {
                values[i]=-2.0*dt*dot(err_xyz,J0_xyz.getCol(i));
                idx++;
            }

            if (hitting_constraints)
            {
                // shoulder's cables length
                values[idx++]=1.71*dt;
                values[idx++]=-1.71*dt;

                values[idx++]=1.71*dt;
                values[idx++]=-1.71*dt;
                values[idx++]=-1.71*dt;

                values[idx++]=dt;
                values[idx++]=dt;

                // avoid hitting torso
                values[idx++]=dt;
                values[idx++]=-shou_m*dt;

                // avoid hitting forearm
                values[idx++]=-elb_m*dt;
                values[idx++]=dt;

                values[idx++]=elb_m*dt;
                values[idx++]=dt;
            }
        }

        return true;
    }

    /****************************************************************/
    void ControllerNLP::finalize_solution(Ipopt::SolverReturn status, Ipopt::Index n,
                           const Ipopt::Number *x, const Ipopt::Number *z_L,
                           const Ipopt::Number *z_U, Ipopt::Index m,
                           const Ipopt::Number *g, const Ipopt::Number *lambda,
                           Ipopt::Number obj_value, const Ipopt::IpoptData *ip_data,
                           Ipopt::IpoptCalculatedQuantities *ip_cq)
    {
        for (Ipopt::Index i=0; i<n; i++)
            v[i]=x[i];
    }


