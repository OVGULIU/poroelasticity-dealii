#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/logstream.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_in.h>
#include <deal.II/grid/grid_out.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/base/tensor_function.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_q.h>

#include <fstream>
#include <iostream>
#include <vector>
#include <array>


namespace PoroElasticity {
  // elastic constants
  double E = 1e6;
  double nu = 0.25;
  double m_modulus = 1e6;
  double biot_coef =0.9;
  double k_drained = 1;
  double permeability = 1e-12;
  double viscosity = 1e-3;
  double time_step = 0.1;
  double r_well = 1;

  double lame_constant = E*nu/((1.+nu)*(1.-2.*nu));
  double shear_modulus = 0.5*E/(1+nu);
  double t_max = 10;

  unsigned int bottom = 0, right = 1, top = 2, left = 3, wellbore = 4;

  // elasticity BC's
  std::vector<unsigned int> displacement_dirichlet_labels =
    {bottom, top, left, right};
  std::vector<unsigned int> displacement_dirichlet_components =
    {1, 1, 0, 0};
  std::vector<double> displacement_dirichlet_values =
    {0, 0, 0, 0};

  std::vector<unsigned int> displacement_neumann_labels = {};
  std::vector<unsigned int> displacement_neumann_components = {};
  std::vector<double>       displacement_neumann_values      = {};

  // pressure BC's
  std::vector<unsigned int> pressure_dirichlet_labels     = {};
  std::vector<unsigned int> pressure_dirichlet_components = {};
  std::vector<double>       pressure_dirichlet_values     = {};

  std::vector<unsigned int> pressure_neumann_labels     = {};
  std::vector<unsigned int> pressure_neumann_components = {};
  std::vector<double>       pressure_neumann_values     = {};

  using namespace dealii;

  // --------------------- Tensor Indexer ------------------------------------
  template <int dim>
  class TensorIndexer {
  public:
    TensorIndexer();
    int tensor_to_component_index(int tensor_index);
    int component_to_tensor_index(int component);
  private:
    std::vector<int> tensor_to_component_index_map;
  };

  template <int dim>
  TensorIndexer<dim>::TensorIndexer()
  {
    switch (dim) {
    case 1:
      tensor_to_component_index_map = {0};
      break;
    case 2:
      tensor_to_component_index_map = {0, 1, 1, 2};
      break;
    case 3:
      tensor_to_component_index_map = {0, 1, 2,
                                       1, 3, 4,
                                       2, 4, 5};
      break;
    default:
      Assert(false, ExcNotImplemented());
    }
  }

  template <int dim>
  int TensorIndexer<dim>::tensor_to_component_index(int tensor_index)
  {
    return tensor_to_component_index_map[tensor_index];
  }
  // --------------------- Right Hand Side -----------------------------------
  template <int dim>
  class DisplacementRightHandSide :  public Function<dim>
  {
  public:
    DisplacementRightHandSide ();

    virtual void vector_value (Vector<double>   &values) const;
    virtual void vector_value_list (const std::vector<Point<dim> > &points,
                                    std::vector<Vector<double> >   &value_list) const;
  };

  template <int dim>
  DisplacementRightHandSide<dim>::DisplacementRightHandSide ()
    :
    Function<dim> (dim)
  {}

  template <int dim>
  inline
  void DisplacementRightHandSide<dim>::vector_value(Vector<double>   &values) const {
    Assert(values.size() == dim,
           ExcDimensionMismatch (values.size(), dim));
    Assert(dim == 2, ExcNotImplemented());

    values(0) = 0;
    values(1) = 0;
  }

  template <int dim>
  void DisplacementRightHandSide<dim>::vector_value_list(const std::vector<Point<dim> > &points,
                                             std::vector<Vector<double> >   &value_list
                                             ) const {
    Assert (value_list.size() == points.size(),
            ExcDimensionMismatch (value_list.size(), points.size()));
    const unsigned int n_points = points.size();
    for (unsigned int p=0; p < n_points; ++p)
      DisplacementRightHandSide<dim>::vector_value (value_list[p]);
  }

  // --------------------- Pressure Source Term ------------------------------
  template<int dim>
  class PressureSourceTerm : public Function<dim>
  {
  public:
    PressureSourceTerm() :
      Function<dim>()
    {}

    virtual double value(const Point<dim> &p,
                         const unsigned int component = 0) const;
  };

  template<int dim>
  double PressureSourceTerm<dim>::value(const Point<dim> &p,
                                        const unsigned int component) const
  {
    Assert (component == 0, ExcInternalError());
    Assert (dim == 2, ExcNotImplemented());

    // if ((p[0] > 1) && (p[1] > -0.5))
    double r_squared = p[0]*p[0] + p[1]*p[1];
    if (r_squared <= r_well*r_well)
      return 1;
    else
      return 0;
  }
  // --------------------- Compute local strain ------------------------------
  template <int dim>
  inline SymmetricTensor<2,dim>
  get_strain (const std::vector<Tensor<1,dim> > &grad)
  {
    Assert (grad.size() == dim, ExcInternalError());
    SymmetricTensor<2,dim> strain;
    for (unsigned int i=0; i<dim; ++i)
      strain[i][i] = grad[i][i];
    for (unsigned int i=0; i<dim; ++i)
      for (unsigned int j=i+1; j<dim; ++j)
        strain[i][j] = (grad[i][j] + grad[j][i]) / 2;
    return strain;
  }

  // --------------------- Boundary Conditions ------------------------------
  template <int dim>
  class BoundaryConditions {
  public:
    // BoundaryConditions();
    // ~BoundaryConditions();
    void set_dirichlet(std::vector<unsigned int> &labels,
                       std::vector<unsigned int> &components,
                       std::vector<double>       &values);
    void set_neumann(std::vector<unsigned int> &labels,
                     std::vector<unsigned int> &components,
                     std::vector<double>       &values);

    std::vector<unsigned int>
      neumann_labels, dirichlet_labels,
      neumann_components, dirichlet_components;
    std::vector<double> dirichlet_values, neumann_values;

    unsigned int n_dirichlet;
    unsigned int n_neumann;
  };

  template <int dim>
  void BoundaryConditions<dim>::set_dirichlet (
   std::vector<unsigned int> &labels,
   std::vector<unsigned int> &components,
   std::vector<double>       &values)
  {
    n_dirichlet = labels.size();
    ExcDimensionMismatch(components.size(), n_dirichlet);
    ExcDimensionMismatch(values.size(), n_dirichlet);

    for (unsigned int d=0; d<components.size(); ++d)
      Assert(components[d] < dim, ExcNotImplemented());

    dirichlet_labels = labels;
    dirichlet_values = values;
    dirichlet_components = components;
  }

  template <int dim>
  void BoundaryConditions<dim>::set_neumann(
     std::vector<unsigned int> &labels,
     std::vector<unsigned int> &components,
     std::vector<double>       &values)
  {
    n_neumann = labels.size();
    ExcDimensionMismatch(components.size(), n_neumann);
    ExcDimensionMismatch(values.size(), n_neumann);

    for (unsigned int d=0; d<components.size(); ++d){
      Assert(components[d] < dim, ExcNotImplemented());
    }

    neumann_labels = labels;
    neumann_values = values;
    neumann_components = components;
  }

  // ---------------------------- Problem ------------------------------------
  template <int dim>
  class PoroElasticProblem {
  public:
    PoroElasticProblem();
    ~PoroElasticProblem();
    void run ();

  private:
    void read_mesh();

    void setup_dofs();
    // void set_boundary_conditions();

    // void assemble_displacement_system();
    // // void solve_displacement_system();

    // void assemble_strain_projection_rhs(std::vector<int> tensor_components);
    // void solve_strain_projection(int component);

    // void get_volumetric_strain();
    // void update_volumetric_strain();
    // void assemble_pressure_residual();
    // void assemble_pressure_jacobian();
    // void solve_pressure_system();

    // void refine_grid();
    // void output_results(const unsigned int cycle) const;
    // void compute_derived_quantities();

    TensorIndexer<dim>   tensor_indexer;
    Triangulation<dim>   triangulation;

    FE_Q<dim>            pressure_fe;
    DoFHandler<dim>      pressure_dof_handler;
    ConstraintMatrix     pressure_constraints;
    SparsityPattern      pressure_sparsity_pattern;
    SparseMatrix<double> pressure_mass_matrix;
    SparseMatrix<double> pressure_laplace_matrix;
    SparseMatrix<double> pressure_jacobian;
    Vector<double>       pressure_solution, old_pressure_solution;
    Vector<double>       pressure_system_rhs;
    std::vector< Vector<double> > pressure_projection_rhs, strains, stresses;

    FESystem<dim>        displacement_fe;
    DoFHandler<dim>      displacement_dof_handler;
    ConstraintMatrix     displacement_constraints;
    SparsityPattern      displacement_sparsity_pattern;
    SparseMatrix<double> displacement_system_matrix;
    Vector<double>       displacement_rhs;
    Vector<double>       displacement_solution;

    // double               time;
    // double               time_step;
    // unsigned int         timestep_number;
  };

  template <int dim>
  PoroElasticProblem<dim>::PoroElasticProblem() :
    displacement_dof_handler(triangulation),
    displacement_fe(FE_Q<dim>(2), dim),
    pressure_dof_handler(triangulation),
    pressure_fe(1)
  {}

  template <int dim>
  PoroElasticProblem<dim>::~PoroElasticProblem()
  {
    pressure_dof_handler.clear();
    displacement_dof_handler.clear();
  }

  template <int dim>
  void PoroElasticProblem<dim>::setup_dofs()
  {
    { // displacement constrains
      displacement_dof_handler.distribute_dofs(displacement_fe);

      displacement_constraints.clear();
      DoFTools::make_hanging_node_constraints(displacement_dof_handler,
                                              displacement_constraints);

      std::vector<ComponentMask> displacement_masks(dim);
      for (unsigned int comp=0; comp<dim; ++comp){
        FEValuesExtractors::Scalar displacement_extractor(comp);
        displacement_masks[comp]
          = displacement_fe.component_mask(displacement_extractor);
      }
    }
    { // pressure constraints
      pressure_dof_handler.distribute_dofs(pressure_fe);
      pressure_constraints.clear();
      DoFTools::make_hanging_node_constraints(pressure_dof_handler,
                                              pressure_constraints);
      pressure_constraints.close();
    }
    // create sparsity patterns, init vectors and matrices
    { // displacement
      unsigned int displacement_n_dofs = displacement_dof_handler.n_dofs();
      DynamicSparsityPattern dsp(displacement_n_dofs);
      DoFTools::make_sparsity_pattern(displacement_dof_handler,
                                      dsp,
                                      displacement_constraints,
                                      /*keep_constrained_dofs = */ true);
      displacement_sparsity_pattern.copy_from(dsp);

      // matrices
      displacement_system_matrix.reinit(displacement_sparsity_pattern);
      displacement_rhs.reinit(displacement_n_dofs);
      displacement_solution.reinit(displacement_n_dofs);
    }
    { // pressure
      unsigned int pressure_n_dofs = pressure_dof_handler.n_dofs();
      DynamicSparsityPattern dsp(pressure_n_dofs);
      DoFTools::make_sparsity_pattern(pressure_dof_handler,
                                      dsp,
                                      pressure_constraints,
                                      /*keep_constrained_dofs = */ true);
      pressure_sparsity_pattern.copy_from(dsp);
    }
  }

  template <int dim>
  void PoroElasticProblem<dim>::run()
  {
    read_mesh();
    setup_dofs();
  }

  template <int dim>
  void PoroElasticProblem<dim>::read_mesh (){
	  GridIn<dim> gridin;
	  gridin.attach_triangulation(triangulation);
	  std::ifstream f("domain.msh");
	  gridin.read_msh(f);
  }
}

int main () {
  try {
    dealii::deallog.depth_console(0);

    PoroElasticity::PoroElasticProblem<2> poro_elastic_problem_2d;
    poro_elastic_problem_2d.run ();
  }

  catch (std::exception &exc) {
    std::cerr << std::endl << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Exception on processing: " << std::endl
              << exc.what() << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;

    return 1;
  }

  catch(...) {
    std::cerr << std::endl << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Unknown exception!" << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;
    return 1;
  }

  return 0;
}
