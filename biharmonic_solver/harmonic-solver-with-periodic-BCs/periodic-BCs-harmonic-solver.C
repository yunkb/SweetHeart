// adapted from libMesh example, miscellaneous/ex5

#include <iostream>

// LibMesh include files.
#include "libmesh/libmesh.h"
#include "libmesh/mesh.h"
#include "libmesh/equation_systems.h"
#include "libmesh/mesh_data.h"
#include "libmesh/mesh_generation.h"
#include "libmesh/mesh_modification.h"
#include "libmesh/elem.h"
#include "libmesh/transient_system.h"
#include "libmesh/fe.h"
#include "libmesh/quadrature_gauss.h"
#include "libmesh/dof_map.h"
#include "libmesh/sparse_matrix.h"
#include "libmesh/dense_matrix.h"
#include "libmesh/dense_vector.h"
#include "libmesh/dense_submatrix.h"
#include "libmesh/dense_subvector.h"
#include "libmesh/numeric_vector.h"
#include "libmesh/linear_implicit_system.h"
#include "libmesh/exodusII_io.h"
#include "libmesh/fe_interface.h"
#include "libmesh/getpot.h"
#include "libmesh/mesh_refinement.h"
#include "libmesh/error_vector.h"
#include "libmesh/kelly_error_estimator.h"
#include "libmesh/discontinuity_measure.h"
#include "libmesh/string_to_enum.h"
#include "libmesh/boundary_info.h"
#include "libmesh/point_locator_base.h"
#include "libmesh/point_locator_list.h"
#include "libmesh/periodic_boundaries.h"
#include "libmesh/periodic_boundary.h"
#include "libmesh/petsc_vector.h"



#include "libmesh/exact_solution.h"

// Bring in everything from the libMesh namespace
using namespace libMesh;


inline bool
is_physical_bdry(const Elem* elem,
        const unsigned short int side,
        const BoundaryInfo& boundary_info,
        const DofMap& dof_map)
{
    const std::vector<short int>& bdry_ids = boundary_info.boundary_ids(elem, side);
    bool at_physical_bdry = !elem->neighbor(side);
    for (std::vector<short int>::const_iterator cit = bdry_ids.begin(); cit != bdry_ids.end(); ++cit)
    {
        if (dof_map.is_periodic_boundary(*cit)) at_physical_bdry = false;
    }
    return at_physical_bdry;
}



void assemble_ipdg_poisson(EquationSystems & es,
        const std::string & system_name)
{
    
    std::cout << "------------------------------"  << std::endl; 
    std::cout << "in IPDG assemble" << std::endl;
    std::cout << "------------------------------"  << std::endl; 
    
    const MeshBase & mesh = es.get_mesh();
    const BoundaryInfo& boundary_info = *mesh.boundary_info;
    const PointLocatorList& point_locator(mesh);
    point_locator.build(TREE_ELEMENTS, mesh);
    if(point_locator.initialized()) { std::cout << "point locator initialized" << std::endl; } 
    const unsigned int dim = mesh.mesh_dimension();
    LinearImplicitSystem& ellipticdg_system = es.get_system<LinearImplicitSystem>("Elliptic");
    const Real ipdg_poisson_penalty = es.parameters.get<Real> ("ipdg_poisson_penalty");
    
    const double epsilon = es.parameters.get<Real>("Phi_epsilon");
    const double epsilon_inv = (std::abs(epsilon) > std::numeric_limits<double>::epsilon() ? 1.0 / epsilon : 0.0);
    
    // had to remove the 'const' qualifier for dof_map because I need info about periodic boundaries
    DofMap& dof_map = ellipticdg_system.get_dof_map();
    PeriodicBoundaries * periodic_boundaries = dof_map.get_periodic_boundaries();
    
    FEType fe_type = ellipticdg_system.variable_type(0);
    
    UniquePtr<FEBase> fe  (FEBase::build(dim, fe_type));
    UniquePtr<FEBase> fe_elem_face(FEBase::build(dim, fe_type));
    UniquePtr<FEBase> fe_neighbor_face(FEBase::build(dim, fe_type));
    
    // Quadrature rules for numerical integration on volumes and faces
    QGauss qrule (dim, fe_type.default_quadrature_order());
    fe->attach_quadrature_rule (&qrule);
    
    QGauss qface(dim-1, fe_type.default_quadrature_order());
    fe_elem_face->attach_quadrature_rule(&qface);
    fe_neighbor_face->attach_quadrature_rule(&qface);
    
    // for volume integrals
    const std::vector<Real> & JxW = fe->get_JxW();
    const std::vector<std::vector<RealGradient> > & dphi = fe->get_dphi();
    const std::vector<std::vector<Real> > &  phi = fe->get_phi();
    
    //  for surface integrals
    const std::vector<std::vector<Real> > &  phi_face = fe_elem_face->get_phi();
    const std::vector<std::vector<RealGradient> > & dphi_face = fe_elem_face->get_dphi();
    const std::vector<Real> & JxW_face = fe_elem_face->get_JxW();
    const std::vector<libMesh::Point> & qface_normals = fe_elem_face->get_normals();
    
    // for surface integrals on the neighbor boundary
    const std::vector<std::vector<Real> > &  phi_neighbor_face = fe_neighbor_face->get_phi();
    const std::vector<std::vector<RealGradient> > & dphi_neighbor_face = fe_neighbor_face->get_dphi();
    
    // local matrices
    DenseMatrix<Number> Ke;
    DenseVector<Number> Fe;
    DenseMatrix<Number> Kne;
    DenseMatrix<Number> Ken;
    DenseMatrix<Number> Kee;
    DenseMatrix<Number> Knn;
    DenseVector<double> rhs_e;
    
    // vector for degree of freedom indices on a particular element
    std::vector<dof_id_type> dof_indices;
    
    // loop over elements
    MeshBase::const_element_iterator el = mesh.active_local_elements_begin();
    const MeshBase::const_element_iterator end_el = mesh.active_local_elements_end();
    
    for ( ; el != end_el; ++el)
    {  
        // Store a pointer to the element we are currently
        // working on.  This allows for nicer syntax later.
        const Elem * elem = *el;
        
        // Get the degree of freedom indices for the
        // current element.  These define where in the global
        // matrix and right-hand-side this element will
        // contribute to.
        dof_map.dof_indices (elem, dof_indices);
        const unsigned int n_dofs = dof_indices.size();
        
        // Compute the element-specific data for the current
        // element.  This involves computing the location of the
        // quadrature points (q_point) and the shape functions
        // (phi, dphi) for the current element.
        fe->reinit (elem);
        
        // Zero the element matrix and right-hand side before
        // summing them.  We use the resize member here because
        // the number of degrees of freedom might have changed from
        // the last element.
        Ke.resize (n_dofs, n_dofs);
        rhs_e.resize(static_cast<int>(dof_indices.size()));
        
        // Now we will build the element interior matrix and the source term.  
        // For the element interior matrix
        // this involves a double loop to integrate the test functions (i) against
        // the trial functions (j).
        for (unsigned int qp=0; qp<qrule.n_points(); qp++)
        {
            for (unsigned int i=0; i<n_dofs; i++)
            {
                for (unsigned int j=0; j<n_dofs; j++)
                {
                    Ke(i,j) += JxW[qp]*(epsilon_inv*phi[i][qp]*phi[j][qp] + dphi[i][qp]*dphi[j][qp]);
                }
            }
        }
        
        
        // The following loops over the sides of the element.
        // if the side is not on the physical boundary
        // then it must either be on a periodic boundary or
        // an interior side.
        for (unsigned int side=0; side<elem->n_sides(); side++)
        {
            if (is_physical_bdry(elem, side, boundary_info, dof_map))
            { 
                // Pointer to the element face
                fe_elem_face->reinit(elem, side);
                
                UniquePtr<Elem> elem_side (elem->build_side(side));
                // h element dimension to compute the interior penalty parameter
                const unsigned int elem_b_order = static_cast<unsigned int> (fe_elem_face->get_order());
                const double h_elem = elem->volume()/elem_side->volume() * 1./pow(elem_b_order, 2.);
                
                for (unsigned int qp=0; qp<qface.n_points(); qp++)
                {
                    for (unsigned int i=0; i<n_dofs; i++)
                    {
                        // Matrix contribution
                        for (unsigned int j=0; j<n_dofs; j++)
                        {
                            // stability
                            Ke(i,j) += JxW_face[qp] * ipdg_poisson_penalty/h_elem * phi_face[i][qp] * phi_face[j][qp];
                            
                            // consistency
                            Ke(i,j) -=
                                    JxW_face[qp] *
                                    (phi_face[i][qp] * (dphi_face[j][qp]*qface_normals[qp]) +
                                    phi_face[j][qp] * (dphi_face[i][qp]*qface_normals[qp]));
                        }
                        
                        
                        if (fabs(qface_normals[qp](1) + 1.0) < 1e2 * std::numeric_limits<Real>::epsilon())
                        {
                            rhs_e(i) += JxW_face[qp] * (1.0) * ipdg_poisson_penalty/h_elem * phi_face[i][qp];
                            rhs_e(i) -= JxW_face[qp] * dphi_face[i][qp] * ((1.0) * qface_normals[qp]);
                        }
                        
                        if (fabs(qface_normals[qp](1) - 1.0) < 1e2 * std::numeric_limits<Real>::epsilon())
                        {
                            rhs_e(i) += JxW_face[qp] * (-2.0) * ipdg_poisson_penalty/h_elem * phi_face[i][qp];
                            rhs_e(i) -= JxW_face[qp] * dphi_face[i][qp] * ((-2.0) * qface_normals[qp]);
                        }
                        
                    }
                }
            }
            
            // element side is either in the interior of the domain or 
            // on a periodic boundary.
            else
            {
                // Store a pointer to the neighbor we are currently
                // working on. if this side is not on the physical boundary,
                // then it must be on a periodic boundary or an interior face
                // between elements.
                const Elem * neighbor = elem->topological_neighbor(side,
                                                                   mesh,
                                                                   point_locator,
                                                                   periodic_boundaries);
          
                // find id of corresponding neighbor side,
                // since elements with periodic boundaries may not share the same
                // side in physical space.
                int blah = 0;
                for (int foo = 0; foo < neighbor->n_sides(); foo++)
                {
                    const Elem *  foo_elem = neighbor->topological_neighbor(foo, mesh, point_locator, periodic_boundaries);
                    
                    if (!(foo_elem == libmesh_nullptr))
                    {
                        if(foo_elem->id() == elem->id()) { blah = foo; }
                    }
                }
                const int neighbor_side = blah;
                                         
                // Get the global id of the element and the neighbor
                const unsigned int elem_id = elem->id();
                const unsigned int neighbor_id = neighbor->id();
                
                // If the neighbor has the same h level and is active
                // perform integration only if our global id is bigger than our neighbor id.
                // We don't want to compute twice the same contributions.
                // If the neighbor has a different h level perform integration
                // only if the neighbor is at a lower level.
                if ((neighbor->active() &&
                        (neighbor->level() == elem->level()) &&
                        (elem_id < neighbor_id)) ||
                        (neighbor->level() < elem->level()))
                {
                    // Pointer to the element side
                    UniquePtr<Elem> elem_side (elem->build_side(side));
                    
                    // h dimension to compute the interior penalty penalty parameter
                    const unsigned int elem_b_order = static_cast<unsigned int>(fe_elem_face->get_order());
                    const unsigned int neighbor_b_order = static_cast<unsigned int>(fe_neighbor_face->get_order());
                    const double side_order = (elem_b_order + neighbor_b_order)/2.;
                    const double h_elem = (elem->volume()/elem_side->volume()) * 1./pow(side_order,2.);
                    
                    // The quadrature point locations on the neighbor side
                    std::vector<libMesh::Point> qface_neighbor_point;
                    std::vector<libMesh::Point> qface_neighbor_ref_point;
                    std::vector<libMesh::Point> temp;
                                                            
                    // Reinitialize shape functions on the element side
                    fe_elem_face->reinit(elem, side);
                    
                    // re init this temporarily...
                    fe_neighbor_face->reinit(neighbor, neighbor_side);
                    
                    // Get the physical locations of the element quadrature points
                    qface_neighbor_point = fe_neighbor_face->get_xyz();
                    
                    // Find their ref locations on the neighbor
                    FEInterface::inverse_map (neighbor->dim(),
                                              fe->get_fe_type(),
                                              neighbor,
                                              qface_neighbor_point,
                                              qface_neighbor_ref_point);
                                        
                    // rearrange ref points on the neighbor side for boundary integration
                    for (int ii = 0; ii < qface.n_points(); ii++)
                    {
                        temp.push_back(qface_neighbor_ref_point[qface.n_points() - 1 - ii]);
                    }
                    
                    // Calculate the neighbor element shape functions at those locations
                    fe_neighbor_face->reinit(neighbor, &temp);
                                       
                    // Get the degree of freedom indices for the
                    // neighbor.  These define where in the global
                    // matrix this neighbor will contribute to.
                    std::vector<dof_id_type> neighbor_dof_indices;
                    dof_map.dof_indices (neighbor, neighbor_dof_indices);
                    const unsigned int n_neighbor_dofs = neighbor_dof_indices.size();
                    
                    // Zero the element and neighbor side matrix before
                    // summing them.  We use the resize member here because
                    // the number of degrees of freedom might have changed from
                    // the last element or neighbor.
                    // Note that Kne and Ken are not square matrices if neighbor
                    // and element have a different p level
                    Kne.resize (n_neighbor_dofs, n_dofs);
                    Ken.resize (n_dofs, n_neighbor_dofs);
                    Kee.resize (n_dofs, n_dofs);
                    Knn.resize (n_neighbor_dofs, n_neighbor_dofs);
                    
                    // Now we will build the element and neighbor
                    // boundary matrices.  This involves
                    // a double loop to integrate the test funcions
                    // (i) against the trial functions (j).
                    for (unsigned int qp=0; qp<qface.n_points(); qp++)
                    {
                        // Kee Matrix. Integrate the element test function i
                        // against the element test function j
                        for (unsigned int i=0; i<n_dofs; i++)
                        {
                            for (unsigned int j=0; j<n_dofs; j++)
                            {
                                // consistency
                                Kee(i,j) -=
                                        0.5 * JxW_face[qp] *
                                        (phi_face[j][qp]*(qface_normals[qp]*dphi_face[i][qp]) +
                                        phi_face[i][qp]*(qface_normals[qp]*dphi_face[j][qp]));
                                
                                // stability
                                Kee(i,j) += JxW_face[qp] * ipdg_poisson_penalty/h_elem * phi_face[j][qp]*phi_face[i][qp];
                            }
                        }
                        
                        // Knn Matrix. Integrate the neighbor test function i
                        // against the neighbor test function j
                        for (unsigned int i=0; i<n_neighbor_dofs; i++)
                        {
                            for (unsigned int j=0; j<n_neighbor_dofs; j++)
                            {
                                // consistency
                                Knn(i,j) +=
                                        0.5 * JxW_face[qp] *
                                        (phi_neighbor_face[j][qp]*(qface_normals[qp]*dphi_neighbor_face[i][qp]) +
                                        phi_neighbor_face[i][qp]*(qface_normals[qp]*dphi_neighbor_face[j][qp]));
                                
                                // stability
                                Knn(i,j) +=
                                        JxW_face[qp] * ipdg_poisson_penalty/h_elem * phi_neighbor_face[j][qp]*phi_neighbor_face[i][qp];
                            }
                        }
                        
                        // Kne Matrix. Integrate the neighbor test function i
                        // against the element test function j
                        for (unsigned int i=0; i<n_neighbor_dofs; i++)
                        {
                            for (unsigned int j=0; j<n_dofs; j++)
                            {
                                // consistency
                                Kne(i,j) +=
                                        0.5 * JxW_face[qp] *
                                        (phi_neighbor_face[i][qp]*(qface_normals[qp]*dphi_face[j][qp]) -
                                        phi_face[j][qp]*(qface_normals[qp]*dphi_neighbor_face[i][qp]));
                                
                                // stability
                                Kne(i,j) -= JxW_face[qp] * ipdg_poisson_penalty/h_elem * phi_face[j][qp]*phi_neighbor_face[i][qp];
                            }
                        }
                        
                        // Ken Matrix. Integrate the element test function i
                        // against the neighbor test function j
                        for (unsigned int i=0; i<n_dofs; i++)
                        {
                            for (unsigned int j=0; j<n_neighbor_dofs; j++)
                            {
                                // consistency
                                Ken(i,j) +=
                                        0.5 * JxW_face[qp] *
                                        (phi_neighbor_face[j][qp]*(qface_normals[qp]*dphi_face[i][qp]) -
                                        phi_face[i][qp]*(qface_normals[qp]*dphi_neighbor_face[j][qp]));
                                
                                // stability
                                Ken(i,j) -= JxW_face[qp] * ipdg_poisson_penalty/h_elem * phi_face[i][qp]*phi_neighbor_face[j][qp];
                            }
                        }
                    }
                    
                    // The element and neighbor boundary matrix are now built
                    // for this side.  Add them to the global matrix
                    // The SparseMatrix::add_matrix() members do this for us.
                    ellipticdg_system.matrix->add_matrix(Kne, neighbor_dof_indices, dof_indices);
                    ellipticdg_system.matrix->add_matrix(Ken, dof_indices, neighbor_dof_indices);
                    ellipticdg_system.matrix->add_matrix(Kee, dof_indices);
                    ellipticdg_system.matrix->add_matrix(Knn, neighbor_dof_indices);
                
                }
            }
        }
        
        
        // The element interior matrix aTensornd right-hand-side are now built
        // for this element.  Add them to the global matrix and
        // right-hand-side vector.  The SparseMatrix::add_matrix()
        // and NumericVector::add_vector() members do this for us.
        ellipticdg_system.matrix->add_matrix(Ke, dof_indices);
        ellipticdg_system.rhs->add_vector(rhs_e,dof_indices);
    }
    
    ellipticdg_system.matrix->close();
    ellipticdg_system.rhs->close();
    
}


void
assemble_cg_poisson(EquationSystems& es, const std::string& /*system_name*/)
{
    
    std::cout << "------------------------------"  << std::endl; 
    std::cout << "in CG assemble" << std::endl;
    std::cout << "------------------------------"  << std::endl; 
    
    const MeshBase& mesh = es.get_mesh();
    const BoundaryInfo& boundary_info = *mesh.boundary_info;
    const unsigned int dim = mesh.mesh_dimension();
    LinearImplicitSystem& system = es.get_system<LinearImplicitSystem>("Elliptic");
    const DofMap& dof_map = system.get_dof_map();
    FEType fe_type = dof_map.variable_type(0);
    
    UniquePtr<FEBase> fe(FEBase::build(dim, fe_type));
    QGauss qrule(dim, FIFTH);
    fe->attach_quadrature_rule(&qrule);
    UniquePtr<FEBase> fe_face(FEBase::build(dim, fe_type));
    QGauss qface(dim - 1, FIFTH);
    fe_face->attach_quadrature_rule(&qface);
    const std::vector<libMesh::Point> & qface_normals = fe_face->get_normals();
    
    
    const std::vector<Real>& JxW = fe->get_JxW();
    const std::vector<std::vector<Real> >& phi = fe->get_phi();
    const std::vector<std::vector<RealGradient> >& dphi = fe->get_dphi();
    
    const std::vector<std::vector<Real> >& phi_face = fe_face->get_phi();
    const std::vector<Real>& JxW_face = fe_face->get_JxW();
    
    DenseMatrix<Number> Ke;
    std::vector<dof_id_type> dof_indices;
    DenseVector<double> rhs_e;
    
    const double epsilon = es.parameters.get<Real>("Phi_epsilon");
    const double epsilon_inv = (std::abs(epsilon) > std::numeric_limits<double>::epsilon() ? 1.0 / epsilon : 0.0);
    
    const Real PENALTY = es.parameters.get<Real> ("cg_poisson_penalty");
    
    MeshBase::const_element_iterator el = mesh.active_local_elements_begin();
    const MeshBase::const_element_iterator end_el = mesh.active_local_elements_end();
    for (; el != end_el; ++el)
    {
        const Elem* elem = *el;
        dof_map.dof_indices(elem, dof_indices);
        fe->reinit(elem);
        unsigned int Ke_size = static_cast<unsigned int>(dof_indices.size());
        Ke.resize(Ke_size, Ke_size);
        rhs_e.resize(static_cast<int>(dof_indices.size()));
        for (unsigned int qp = 0; qp < qrule.n_points(); qp++)
        {
            for (unsigned int i = 0; i < phi.size(); i++)
            {
                for (unsigned int j = 0; j < phi.size(); j++)
                {
                    Ke(i, j) += (epsilon_inv * phi[i][qp] * phi[j][qp] + (dphi[i][qp] * dphi[j][qp])) * JxW[qp];
                }
            }
        }
        for (unsigned int side = 0; side < elem->n_sides(); side++)
        {
            
            if (is_physical_bdry(elem, side, boundary_info, dof_map))
            {
                fe_face->reinit(elem, side);
                
                for (unsigned int qp = 0; qp < qface.n_points(); qp++)
                {
                    
                    for (unsigned int i = 0; i < phi_face.size(); ++i)
                    {
                        for (unsigned int j = 0; j < phi_face.size(); ++j)
                        {
                            Ke(i, j) += PENALTY * phi_face[i][qp] * phi_face[j][qp] * JxW_face[qp];
                        }
                        
                        if (fabs(qface_normals[qp](1) + 1.0) < 1e2 * std::numeric_limits<Real>::epsilon())
                        {
                            rhs_e(i) += PENALTY * 1.0 * phi_face[i][qp] * JxW_face[qp];  
                        }
                        
                        if (fabs(qface_normals[qp](1) - 1.0) < 1e2 * std::numeric_limits<Real>::epsilon())
                        {
                            rhs_e(i) += PENALTY * (-2.0) * phi_face[i][qp] * JxW_face[qp];  
                        }
                        
                    }
                }
                
                // Apply constraints (e.g., enforce periodic boundary conditions)
                // and add the elemental contributions to the global vector.
                 dof_map.constrain_element_vector(rhs_e, dof_indices);
                 system.rhs->add_vector(rhs_e, dof_indices); 
                
            }
            
                           
        }
        
        dof_map.constrain_element_matrix(Ke, dof_indices);
        system.matrix->add_matrix(Ke, dof_indices);
    }
    
    system.matrix->close();
    system.rhs->close();
    
}


int main (int argc, char** argv)
{
    LibMeshInit init(argc, argv);
    
    
    // Skip adaptive examples on a non-adaptive libMesh build
#ifndef LIBMESH_ENABLE_AMR
    libmesh_example_requires(false, "--enable-amr");
#else
    
    //Parse the input file
    GetPot input_file("periodic-BCs-harmonic-solver.in");
    
    //Read in parameters from the input file
    Order p_order                                = static_cast<Order>(input_file("p_order", 1));
    const Real ipdg_poisson_penalty              = input_file("ipdg_poisson_penalty", 2.0);
    const Real cg_poisson_penalty                = input_file("cg_poisson_penalty", 1E10);
    const unsigned int NDIM                      = input_file("dimension", 2);
    const std::string solver                     = input_file("poisson_solver", "CG");
    
    // Skip higher-dimensional examples on a lower-dimensional libMesh build
    libmesh_example_requires(NDIM <= LIBMESH_DIM, "2D/3D support");
    
    // Note that boundary condition data must be registered with each FE
    // system before calling IBFEMethod::initializeFEData().
    Mesh mesh(init.comm(), NDIM);
    const double R = 0.25;
    const double w = 0.0625;
    const double dx0 = 1.0 / 64.0;
    const double dx = 1.0 / 128.0 ;
    const double MFAC = input_file("MFAC", 1.);
    const double ds = MFAC * dx;
    std::string elem_type = "QUAD9";
    bool nested_meshes = input_file("CONVERGENCE_STUDY", false);
    const int n_x = nested_meshes ? round(16.0 * round(2.0 * M_PI * R / dx0 / 16.0) / MFAC) * round(dx0 / dx) : ceil(2.0 * M_PI * R / ds);
    const int n_y = nested_meshes ? round(4.0 * round(w / dx0 / 4.0) / MFAC) * round(dx0 / dx) : ceil(w / ds);
    MeshTools::Generation::build_square(mesh, n_x, n_y, 0.0, 2.0 * M_PI * R, 0.0, w, Utility::string_to_enum<ElemType>(elem_type));
    VectorValue<double> boundary_translation(2.0 * M_PI * R, 0.0, 0.0);
    PeriodicBoundary pbc(boundary_translation);
    pbc.myboundary = 3;
    pbc.pairedboundary = 1;
    
    // Create an equation system object
    EquationSystems equation_system (mesh);
    
    // Set parameters for the equation system and the solver
    equation_system.parameters.set<Real>("linear solver tolerance") = TOLERANCE * TOLERANCE;
    equation_system.parameters.set<unsigned int>("linear solver maximum iterations") = 1000;
    
    equation_system.parameters.set<Real>("Phi_epsilon") = 0.0;
    equation_system.parameters.set<Real>("ipdg_poisson_penalty") = ipdg_poisson_penalty;
    equation_system.parameters.set<Real>("cg_poisson_penalty") = cg_poisson_penalty;
    
    // Create a system named ellipticdg
    LinearImplicitSystem & harmonic_system = equation_system.add_system<LinearImplicitSystem> ("Elliptic");
    
    // add periodic boundary conditions
    harmonic_system.get_dof_map().add_periodic_boundary(pbc);
    
    // Add a variable "u" to using the p_order specified in the config file
    if (solver.compare("IPDG") == 0)
    {       
        harmonic_system.add_variable ("u", p_order, MONOMIAL);
        harmonic_system.attach_assemble_function (assemble_ipdg_poisson); 
        harmonic_system.init();
        harmonic_system.solve();
    }
    else if(solver.compare("CG") == 0)
    {
        harmonic_system.add_variable ("u", p_order, LAGRANGE);
        harmonic_system.attach_assemble_function (assemble_cg_poisson);
        harmonic_system.init();
        harmonic_system.solve();
        harmonic_system.get_dof_map().enforce_constraints_exactly(harmonic_system); 
    }
    else
    {
        harmonic_system.add_variable ("u", p_order, LAGRANGE);
        harmonic_system.attach_assemble_function (assemble_cg_poisson);
        harmonic_system.init();
        harmonic_system.solve();
    }
    
    
    
    
    
#ifdef LIBMESH_HAVE_EXODUS_API
    if(solver.compare("IPDG") == 0 )
    {
        ExodusII_IO (mesh).write_discontinuous_exodusII("ipdg-harmonic-periodic-bcs.e", equation_system);
    }
    else if(solver.compare("CG") == 0 )
    {
        ExodusII_IO (mesh).write_equation_systems("cg-harmonic-periodic-bcs.e", equation_system);
    }
    else
    {
        ExodusII_IO (mesh).write_equation_systems("cg-harmonic-periodic-bcs.e", equation_system);
    }
#endif
    
#endif // #ifndef LIBMESH_ENABLE_AMR
    
    // All done.
    return 0;
}
