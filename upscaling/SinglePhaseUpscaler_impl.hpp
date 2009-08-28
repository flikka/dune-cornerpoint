//===========================================================================
//
// File: SinglePhaseUpscaler_impl.hpp
//
// Created: Fri Aug 28 13:46:07 2009
//
// Author(s): Atgeirr F Rasmussen <atgeirr@sintef.no>
//            B�rd Skaflestad     <bard.skaflestad@sintef.no>
//
// $Date$
//
// $Revision$
//
//===========================================================================

/*
  Copyright 2009 SINTEF ICT, Applied Mathematics.
  Copyright 2009 Statoil ASA.

  This file is part of The Open Reservoir Simulator Project (OpenRS).

  OpenRS is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OpenRS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OpenRS.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPENRS_SINGLEPHASEUPSCALER_IMPL_HEADER
#define OPENRS_SINGLEPHASEUPSCALER_IMPL_HEADER


#include <dune/solvers/common/setupGridAndProps.hpp>
#include <dune/solvers/common/setupBoundaryConditions.hpp>


namespace Dune
{

    inline SinglePhaseUpscaler::SinglePhaseUpscaler()
	: bctype_(Fixed),
	  twodim_hack_(false),
	  residual_tolerance_(1e-8)
    {
    }




    inline void SinglePhaseUpscaler::init(const parameter::ParameterGroup& param)
    {
	// Get the bc type parameter early, since we will fake some
	// others depending on it.
        int bct = param.get<int>("boundary_condition_type");
	bctype_ = static_cast<BoundaryConditionType>(bct);
	twodim_hack_ = param.getDefault("2d_hack", false);
	residual_tolerance_ = param.getDefault("residual_tolerance", residual_tolerance_);

	// Faking some parameters depending on bc type.
	parameter::ParameterGroup temp_param = param;
	std::string true_if_periodic = bctype_ == Periodic ? "true" : "false";
	std::tr1::shared_ptr<parameter::ParameterMapItem> use_unique(new parameter::Parameter(true_if_periodic, "bool"));
	std::tr1::shared_ptr<parameter::ParameterMapItem> per_ext(new parameter::Parameter(true_if_periodic, "bool"));
	if (!temp_param.has("use_unique_boundary_ids")) {
	    temp_param.insert("use_unique_boundary_ids", use_unique);
	}
	if (!temp_param.has("periodic_extension")) {
	    temp_param.insert("periodic_extension", per_ext);
	}

	setupGridAndProps(temp_param, grid_, res_prop_);
	ginterf_.init(grid_);

	// Write any unused parameters.
	std::cout << "====================   Unused parameters:   ====================\n";
	param.displayUsage();
	std::cout << "================================================================\n";
    }




    inline const SinglePhaseUpscaler::GridInterface&
    SinglePhaseUpscaler::grid() const
    {
	return ginterf_;
    }




    inline SinglePhaseUpscaler::permtensor_t
    SinglePhaseUpscaler::upscaleSinglePhase()
    {
	int num_cells = ginterf_.numberOfCells();
	// No source or sink.
	std::vector<double> src(num_cells, 0.0);
	// Just water.
	std::vector<double> sat(num_cells, 1.0);
	// Gravity.
	FieldVector<double, 3> gravity(0.0);
	// gravity[2] = -Dune::unit::gravity;

	permtensor_t upscaled_K(3, 3, (double*)0);
	for (int pdd = 0; pdd < Dimension; ++pdd) {
	    setupUpscalingConditions(ginterf_, bctype_, pdd, 1.0, 1.0, twodim_hack_, bcond_);
	    if (pdd == 0) {
		// Only on first iteration, since we do not change the
		// structure of the system, the way the flow solver is
		// implemented.
		flow_solver_.init(ginterf_, res_prop_, bcond_);
	    }

	    // Run pressure solver.
	    flow_solver_.solve(res_prop_, sat, bcond_, src, gravity, residual_tolerance_);

	    // Check and fix fluxes.
// 	    flux_checker_.checkDivergence(grid_, wells, flux);
// 	    flux_checker_.fixFlux(grid_, wells, boundary_, flux);

	    // Compute upscaled K.
	    double Q[Dimension];
	    switch (bctype_) {
	    case Fixed:
		std::fill(Q, Q+Dimension, 0); // resetting Q
		Q[pdd] = computeAverageVelocity(flow_solver_.getSolution(), pdd, pdd);
		break;
	    case Linear:
	    case Periodic:
		for (int i = 0; i < Dimension; ++i) {
		    Q[i] = computeAverageVelocity(flow_solver_.getSolution(), i, pdd);
		}
		break;
	    default:
		THROW("Unknown boundary type: " << bctype_);
	    }
	    double delta = computeDelta(pdd);
	    for (int i = 0; i < Dimension; ++i) {
		upscaled_K(i, pdd) = Q[i] * delta;
	    }
	}
	upscaled_K *= res_prop_.viscosityFirstPhase();
	return upscaled_K;
    }




    template <class FlowSol>
    double SinglePhaseUpscaler::computeAverageVelocity(const FlowSol& flow_solution,
					    const int flow_dir,
					    const int pdrop_dir) const
    {
	double side1_flux = 0.0;
	double side2_flux = 0.0;
	double side1_area = 0.0;
	double side2_area = 0.0;

	int num_faces = 0;
	int num_bdyfaces = 0;
	int num_side1 = 0;
	int num_side2 = 0;

	for (CellIter c = ginterf_.cellbegin(); c != ginterf_.cellend(); ++c) {
	    for (FaceIter f = c->facebegin(); f != c->faceend(); ++f) {
		++num_faces;
		if (f->boundary()) {
		    ++num_bdyfaces;
		    int canon_bid = bcond_.getCanonicalBoundaryId(f->boundaryId());
		    if ((canon_bid - 1)/2 == flow_dir) {
			double flux = flow_solution.outflux(f);
			double area = f->area();
			double norm_comp = f->normal()[flow_dir];
			// std::cout << "bid " << f->boundaryId() << "   area " << area << "   n " << norm_comp << std::endl;
			if (canon_bid - 1 == 2*flow_dir) {
			    ++num_side1;
			    if (flow_dir == pdrop_dir && flux > 0.0) {
				std::cerr << "Flow may be in wrong direction at bid: " << f->boundaryId()
					  << " Magnitude: " << std::fabs(flux) << std::endl;
				// THROW("Detected outflow at entry face: " << face);
			    }
			    side1_flux += flux*norm_comp;
			    side1_area += area;
			} else {
			    ASSERT(canon_bid - 1 == 2*flow_dir + 1);
			    ++num_side2;
			    if (flow_dir == pdrop_dir && flux < 0.0) {
				std::cerr << "Flow may be in wrong direction at bid: " << f->boundaryId()
					  << " Magnitude: " << std::fabs(flux) << std::endl;
				// THROW("Detected inflow at exit face: " << face);
			    }
			    side2_flux += flux*norm_comp;
			    side2_area += area;
			}
		    }		    
		}
	    }
	}
// 	std::cout << "Faces: " << num_faces << "   Boundary faces: " << num_bdyfaces
// 		  << "   Side 1 faces: " << num_side1 << "   Side 2 faces: " << num_side2 << std::endl;
	// q is the average velocity.
	return 0.5*(side1_flux/side1_area + side2_flux/side2_area);
    }




    inline double SinglePhaseUpscaler::computeDelta(const int flow_dir) const
    {
	double side1_pos = 0.0;
	double side2_pos = 0.0;
	double side1_area = 0.0;
	double side2_area = 0.0;
	for (CellIter c = ginterf_.cellbegin(); c != ginterf_.cellend(); ++c) {
	    for (FaceIter f = c->facebegin(); f != c->faceend(); ++f) {
		if (f->boundary()) {
		    int canon_bid = bcond_.getCanonicalBoundaryId(f->boundaryId());
		    if ((canon_bid - 1)/2 == flow_dir) {
			double area = f->area();
			double pos_comp = f->centroid()[flow_dir];
			if (canon_bid - 1 == 2*flow_dir) {
			    side1_pos += area*pos_comp;
			    side1_area += area;
			} else {
			    side2_pos += area*pos_comp;
			    side2_area += area;
			}
		    }		    
		}
	    }
	}
	// delta is the average length.
	return  side2_pos/side2_area - side1_pos/side1_area;
    }

} // namespace Dune



#endif // OPENRS_SINGLEPHASEUPSCALER_IMPL_HEADER