// #include <fstream>
#include <plan_manage/planner_manager.h>
#include <thread>
#include "visualization_msgs/Marker.h" // zx-todo

namespace ego_planner
{

  // SECTION interfaces for setup and query

  EGOPlannerManager::EGOPlannerManager() {}

  EGOPlannerManager::~EGOPlannerManager() { std::cout << "des manager" << std::endl; }

  void EGOPlannerManager::saveSummarizedResult(ros::Time start, ros::Time end, double distance, double max_v)
  {
    double mission_time = (end - start).toSec();
    double dist = distance;
    double max_vel = max_v;
    std::cout << "mission time" << mission_time << std::endl;
    std::cout << "plan_manager receive finished" << std::endl;
    std::string file_name = package_path + "/log/summary_" + scenario + ".csv";
    std::ifstream result_csv_in(file_name);
    bool print_description = false;
    if (not result_csv_in or result_csv_in.peek() == std::ifstream::traits_type::eof())
    {
      print_description = true;
    }

    std::ofstream result_csv_out;
    result_csv_out.open(file_name, std::ios_base::app);
    // if(print_description){
    //     result_csv_out << "drone_id,min_planning_time,max_planning_time, avg_planning_time,average_init_time,average_opt_time\n";
    //                   //  << "average_planning_time,min_planning_time,max_planning_time,"
    //                   //  << "initial_traj_planning_time,obstacle_prediction_time,goal_planning_time,"
    //                   //  << "lsc_generation_time,sfc_generation_time,traj_optimization_time,"
    //                   //  << "mission_file_name,world_file_name,planner_mode,prediction_mode,initial_traj_mode,"
    //                   //  << "slack_mode,goal_mode,world_dimension,dt,horizon,N_constraint_segments\n";
    // }

    result_csv_out << pp_.drone_id << ","
                   << min_time_total << ","
                   << max_time_total << ","
                   << average_time_total << ","
                   << init_time << ","
                   << opt_time << ","
                   << mission_time << ","
                   << dist << ","
                   << max_vel << "\n";
    result_csv_out.close();
  }

  void EGOPlannerManager::initPlanModules(ros::NodeHandle &nh, PlanningVisualization::Ptr vis)
  {
    /* read algorithm parameters */

    nh.param("manager/max_vel", pp_.max_vel_, -1.0);
    nh.param("manager/max_acc", pp_.max_acc_, -1.0);
    nh.param("manager/feasibility_tolerance", pp_.feasibility_tolerance_, 0.0);
    nh.param("manager/polyTraj_piece_length", pp_.polyTraj_piece_length, -1.0);
    nh.param("manager/planning_horizon", pp_.planning_horizen_, 5.0);
    nh.param("manager/use_multitopology_trajs", pp_.use_multitopology_trajs, false);
    nh.param("manager/drone_id", pp_.drone_id, -1);
    nh.param<std::string>("manager/scenario", scenario, "none");

    // zt raynor: GridMap is set by calling EGOPlannerManager::setEnvironment
    // grid_map_.reset(new GridMap);
    // grid_map_->initMap(nh);

    ploy_traj_opt_.reset(new PolyTrajOptimizer);
    ploy_traj_opt_->setParam(nh);
    ploy_traj_opt_->setEnvironment(grid_map_);

    visualization_ = vis;

    ploy_traj_opt_->setSwarmTrajs(&traj_.swarm_traj);
    ploy_traj_opt_->setDroneId(pp_.drone_id);
    package_path = ros::package::getPath("ego_planner");
    sim_start_time = ros::Time::now();
    file_name_time = std::to_string(sim_start_time.toSec());
    mission_finished = false;
    min_time_total = SP_INFINITY;
    max_time_total = 0;
    average_time_total = 0;
    init_time = 0;
    opt_time = 0;
  }

  // zt raynor
  void EGOPlannerManager::setEnvironment(const GridMap::Ptr &map)
  {
    grid_map_ = map;
  }

  void EGOPlannerManager::setGraphSearch(const AStar::Ptr &a_star)
  {
    a_star_ = a_star;
  }

  void EGOPlannerManager::setCorridorGen(const std::shared_ptr<CorridorGen::CorridorGenerator> &corridor_gen)
  {
    corridor_gen_ = corridor_gen;
  }

  bool EGOPlannerManager::reboundReplan(
      const Eigen::Vector3d &start_pt, const Eigen::Vector3d &start_vel,
      const Eigen::Vector3d &start_acc, const Eigen::Vector3d &local_target_pt,
      const Eigen::Vector3d &local_target_vel, const bool flag_polyInit,
      const bool flag_randomPolyTraj, const bool touch_goal)
  {
    ros::Time t_start = ros::Time::now();
    ros::Duration t_init, t_opt;

    static int count = 0;
    cout << "\033[47;30m\n[" << t_start << "] Drone " << pp_.drone_id << " Replan " << count++ << "\033[0m" << endl;
    cout.precision(3);
    cout << "start: " << start_pt.transpose() << ", " << start_vel.transpose() << "\ngoal:" << local_target_pt.transpose() << ", " << local_target_vel.transpose()
         << endl;
    if ((start_pt - local_target_pt).norm() < 0.2)
      cout << "Close to goal" << endl;

    /*** STEP 1: INIT ***/
    ploy_traj_opt_->setIfTouchGoal(touch_goal);
    double ts = pp_.polyTraj_piece_length / pp_.max_vel_;

    poly_traj::MinJerkOpt initMJO;
    if (!computeInitState(start_pt, start_vel, start_acc, local_target_pt, local_target_vel,
                          flag_polyInit, flag_randomPolyTraj, ts, initMJO))
    {
      return false;
    }

    Eigen::MatrixXd cstr_pts = initMJO.getInitConstraintPoints(ploy_traj_opt_->get_cps_num_prePiece_());
    vector<std::pair<int, int>> segments;
    if (ploy_traj_opt_->finelyCheckAndSetConstraintPoints(segments, initMJO, true) == PolyTrajOptimizer::CHK_RET::ERR)
    {
      return false;
    }

    t_init = ros::Time::now() - t_start;

    std::vector<Eigen::Vector3d> point_set;
    for (int i = 0; i < cstr_pts.cols(); ++i)
      point_set.push_back(cstr_pts.col(i));
    visualization_->displayInitPathList(point_set, 0.2, 0);

    t_start = ros::Time::now();

    /*** STEP 2: OPTIMIZE ***/
    bool flag_success = false;
    vector<vector<Eigen::Vector3d>> vis_trajs;
    poly_traj::MinJerkOpt best_MJO;

    // ROS_ERROR("BBBB");

    if (pp_.use_multitopology_trajs)
    {
      std::vector<ConstraintPoints> trajs = ploy_traj_opt_->distinctiveTrajs(segments);
      Eigen::VectorXi success = Eigen::VectorXi::Zero(trajs.size());
      poly_traj::Trajectory initTraj = initMJO.getTraj();
      int PN = initTraj.getPieceNum();
      Eigen::MatrixXd all_pos = initTraj.getPositions();
      Eigen::MatrixXd innerPts = all_pos.block(0, 1, 3, PN - 1);
      Eigen::Matrix<double, 3, 3> headState, tailState;
      headState << initTraj.getJuncPos(0), initTraj.getJuncVel(0), initTraj.getJuncAcc(0);
      tailState << initTraj.getJuncPos(PN), initTraj.getJuncVel(PN), initTraj.getJuncAcc(PN);
      double final_cost, min_cost = 999999.0;

      for (int i = trajs.size() - 1; i >= 0; i--)
      {
        ploy_traj_opt_->setConstraintPoints(trajs[i]);
        ploy_traj_opt_->setUseMultitopologyTrajs(true);
        if (ploy_traj_opt_->optimizeTrajectory(headState, tailState,
                                               innerPts, initTraj.getDurations(), final_cost))
        {
          success[i] = true;

          if (final_cost < min_cost)
          {
            min_cost = final_cost;
            best_MJO = ploy_traj_opt_->getMinJerkOpt();
            flag_success = true;
          }

          // visualization
          Eigen::MatrixXd ctrl_pts_temp = ploy_traj_opt_->getMinJerkOpt().getInitConstraintPoints(ploy_traj_opt_->get_cps_num_prePiece_());
          std::vector<Eigen::Vector3d> point_set;
          for (int j = 0; j < ctrl_pts_temp.cols(); j++)
          {
            point_set.push_back(ctrl_pts_temp.col(j));
          }
          vis_trajs.push_back(point_set);
        }
      }

      t_opt = ros::Time::now() - t_start;

      if (trajs.size() > 1)
      {
        cout << "\033[1;33m"
             << "multi-trajs=" << trajs.size() << ",\033[1;0m"
             << " Success:fail=" << success.sum() << ":" << success.size() - success.sum() << endl;
      }

      visualization_->displayMultiOptimalPathList(vis_trajs, 0.1); // This visuallization will take up several milliseconds.
    }
    else
    {
      poly_traj::Trajectory initTraj = initMJO.getTraj();
      int PN = initTraj.getPieceNum();
      Eigen::MatrixXd all_pos = initTraj.getPositions();
      Eigen::MatrixXd innerPts = all_pos.block(0, 1, 3, PN - 1);
      Eigen::Matrix<double, 3, 3> headState, tailState;
      headState << initTraj.getJuncPos(0), initTraj.getJuncVel(0), initTraj.getJuncAcc(0);
      tailState << initTraj.getJuncPos(PN), initTraj.getJuncVel(PN), initTraj.getJuncAcc(PN);
      double final_cost;
      flag_success = ploy_traj_opt_->optimizeTrajectory(headState, tailState,
                                                        innerPts, initTraj.getDurations(), final_cost);
      best_MJO = ploy_traj_opt_->getMinJerkOpt();

      t_opt = ros::Time::now() - t_start;
    }

    /*** STEP 3: Store and display results ***/
    // cout << "Success=" << (flag_success ? "yes" : "no") << endl;
    if (flag_success)
    {
      static double sum_time = 0;
      static double sum_time_init = 0;
      static double sum_time_opt = 0;
      static int count_success = 0;
      sum_time += (t_init + t_opt).toSec();
      sum_time_init += (t_init).toSec();
      sum_time_opt += (t_opt).toSec();
      count_success++;
      double plan_time_iter = (t_init + t_opt).toSec();
      average_time_total = sum_time / count_success;

      init_time = sum_time_init / count_success;
      opt_time = sum_time_opt / count_success;

      if (plan_time_iter > max_time_total)
      {
        max_time_total = plan_time_iter;
      }

      if (plan_time_iter < min_time_total)
      {
        min_time_total = plan_time_iter;
      }
      printf("Time:\033[42m%.3fms,\033[0m init:%.3fms, optimize:%.3fms, avg=%.3fms\n",
             //  (t_init + t_opt).toSec() * 1000, t_init.toSec() * 1000, t_opt.toSec() * 1000, sum_time / count_success * 1000);
             plan_time_iter * 1000, t_init.toSec() * 1000, t_opt.toSec() * 1000, average_time_total * 1000);
      // cout << "total time:\033[42m" << (t_init + t_opt).toSec()
      //      << "\033[0m,init:" << t_init.toSec()
      //      << ",optimize:" << t_opt.toSec()
      //      << ",avg_time=" << sum_time / count_success << endl;

      setLocalTrajFromOpt(best_MJO, touch_goal);
      cstr_pts = best_MJO.getInitConstraintPoints(ploy_traj_opt_->get_cps_num_prePiece_());
      visualization_->displayOptimalList(cstr_pts, 0);

      continous_failures_count_ = 0;
    }
    else
    {
      cstr_pts = ploy_traj_opt_->getMinJerkOpt().getInitConstraintPoints(ploy_traj_opt_->get_cps_num_prePiece_());
      visualization_->displayFailedList(cstr_pts, 0);

      continous_failures_count_++;
    }

    return flag_success;
  }

  bool EGOPlannerManager::computeInitState(
      const Eigen::Vector3d &start_pt, const Eigen::Vector3d &start_vel, const Eigen::Vector3d &start_acc,
      const Eigen::Vector3d &local_target_pt, const Eigen::Vector3d &local_target_vel,
      const bool flag_polyInit, const bool flag_randomPolyTraj, const double &ts,
      poly_traj::MinJerkOpt &initMJO)
  {

    // ROS_WARN("Call EGOPlannerManager::computeInitState");

    static bool flag_first_call = true;

    // if (flag_first_call || flag_polyInit) /*** case 1: polynomial initialization ***/
    if (true)
    {
      flag_first_call = false;

      /* basic params */
      Eigen::Matrix3d headState, tailState;
      Eigen::MatrixXd innerPs;
      Eigen::VectorXd piece_dur_vec;
      int piece_nums;
      constexpr double init_of_init_totaldur = 2.0;
      headState << start_pt, start_vel, start_acc;
      tailState << local_target_pt, local_target_vel, Eigen::Vector3d::Zero();

      double distance = (local_target_pt - start_pt).norm();
      // ROS_WARN("distance between local target point to start point is:");
      // std::cout << distance << std::endl;

      if (distance < 1)
      {
        piece_nums = 1;
        piece_dur_vec.resize(1);
        piece_dur_vec(0) = 1;
        initMJO.reset(headState, tailState, piece_nums);
        initMJO.generate(innerPs, piece_dur_vec);

        return true;
      }

      else if (continous_failures_count_ > 0)
      {
        Eigen::Vector3d horizen_dir = ((start_pt - local_target_pt).cross(Eigen::Vector3d(0, 0, 1))).normalized();
        Eigen::Vector3d vertical_dir = ((start_pt - local_target_pt).cross(horizen_dir)).normalized();
        innerPs.resize(3, 1);
        innerPs = (start_pt + local_target_pt) / 2 +
                  (((double)rand()) / RAND_MAX - 0.5) *
                      (start_pt - local_target_pt).norm() *
                      horizen_dir * 0.8 * (-0.978 / (continous_failures_count_ + 0.989) + 0.989) +
                  (((double)rand()) / RAND_MAX - 0.5) *
                      (start_pt - local_target_pt).norm() *
                      vertical_dir * 0.4 * (-0.978 / (continous_failures_count_ + 0.989) + 0.989);

        piece_nums = 2;
        piece_dur_vec.resize(2);
        piece_dur_vec = Eigen::Vector2d(init_of_init_totaldur / 2, init_of_init_totaldur / 2);

        initMJO.reset(headState, tailState, piece_nums);
        initMJO.generate(innerPs, piece_dur_vec);
        poly_traj::Trajectory initTraj = initMJO.getTraj();

        /* generate the real init trajectory */
        piece_nums = round((headState.col(0) - tailState.col(0)).norm() / pp_.polyTraj_piece_length);
        if (piece_nums < 2)
          piece_nums = 2;
        double piece_dur = init_of_init_totaldur / (double)piece_nums;
        piece_dur_vec.resize(piece_nums);
        piece_dur_vec = Eigen::VectorXd::Constant(piece_nums, ts);
        innerPs.resize(3, piece_nums - 1);
        int id = 0;
        double t_s = piece_dur, t_e = init_of_init_totaldur - piece_dur / 2;
        for (double t = t_s; t < t_e; t += piece_dur)
        {
          innerPs.col(id++) = initTraj.getPos(t);
        }
        if (id != piece_nums - 1)
        {
          ROS_ERROR("Should not happen! x_x");
          return false;
        }
        initMJO.reset(headState, tailState, piece_nums);
        initMJO.generate(innerPs, piece_dur_vec);

        return true;
      }

      else
      {
        // zt: raynor, conduct A* search for collision-free path
        // A* search
        // Corridor gen
        // trajectory gen

        std::vector<Eigen::Vector3d> path_list;
        ASTAR_RET ret = a_star_->AstarSearch(grid_map_->getResolution(), start_pt, local_target_pt);

        switch (ret)
        {
        case ASTAR_RET::SUCCESS:
          /* code */
          {
            path_list = a_star_->getPath();
            break;
          }

        case ASTAR_RET::SEARCH_ERR:
        {
          ROS_ERROR("Astar search error.");
          return false;
        }

        default:
          break;
        }

        std::vector<std::vector<Eigen::Vector3d>> path_list_vect;
        path_list_vect.push_back(path_list);

        visualization_->displayAStarList(path_list_vect, 0);

        bool corridor_generation_status = corridor_gen_->generateCorridorAlongPath(path_list);
        std::vector<CorridorGen::Corridor> corridor_list_ = corridor_gen_->getCorridor();
        std::vector<Eigen::Vector3d> corridor_waypts; // includes head, intermediate waypoints, tail
        corridor_waypts = corridor_gen_->getWaypointList();

        visualization_->displayCorridor(corridor_list_);

        // ROS_WARN("corridor_waypts");
        // std::cout << corridor_waypts.size() << std::endl;

        std::vector<Eigen::Vector3d> inner_points;
        Eigen::MatrixXd inner_points_mat;
        inner_points_mat.resize(3, corridor_waypts.size() - 2);
        for (int i = 1; i < corridor_waypts.size() - 1; i++)
        {
          inner_points.push_back(corridor_waypts[i]);
          inner_points_mat.col(i - 1) = corridor_waypts[i];
        }

        // ROS_WARN("inner_points_mat setup");
        // std::cout << inner_points_mat << std::endl;

        piece_nums = inner_points.size() + 1;

        double des_vel = pp_.max_vel_ / 1.5;
        piece_dur_vec.resize(piece_nums);

        std::vector<Eigen::Vector3d> points_for_time_allocation;
        for (int i = 1; i < corridor_waypts.size(); i++)
        {
          points_for_time_allocation.push_back(corridor_waypts[i]);
        }

        piece_dur_vec = Eigen::VectorXd::Constant(piece_nums, 1.0);

        // ROS_WARN("time allocated");

        // std::cout << "inner_points_mat num: " << inner_points_mat.cols() << std::endl;
        // std::cout << "piece_dur_vec size: " << piece_dur_vec.size() << std::endl;
        // std::cout << "time allocation: " << piece_dur_vec.transpose() << std::endl;

        initMJO.reset(headState, tailState, piece_nums);

        initMJO.generate(inner_points_mat, piece_dur_vec);

        return true;

        for (int j = 0; j < 2; ++j)
        {
          for (size_t i = 0; i < points_for_time_allocation.size(); ++i)
          {
            piece_dur_vec(i) = (i == 0) ? (points_for_time_allocation[0] - start_pt).norm() / des_vel
                                        : (points_for_time_allocation[i] - points_for_time_allocation[i - 1]).norm() / des_vel;
          }
          // ROS_WARN("time allocated");

          // std::cout << "inner_points_mat num: " << inner_points_mat.cols() << std::endl;
          // std::cout << "piece_dur_vec size: " << piece_dur_vec.size() << std::endl;
          // std::cout << "time allocation: " << piece_dur_vec.transpose() << std::endl;

          initMJO.generate(inner_points_mat, piece_dur_vec);

          // ROS_WARN("initMJO generate");

          if (initMJO.getTraj().getMaxVelRate() < pp_.max_vel_ ||
              start_vel.norm() > pp_.max_vel_ ||
              local_target_vel.norm() > pp_.max_vel_)
          {
            break;
          }

          if (j == 2)
          {
            ROS_WARN("Global traj MaxVel = %f > set_max_vel", initMJO.getTraj().getMaxVelRate());
            cout << "headState=" << endl
                 << headState << endl;
            cout << "tailState=" << endl
                 << tailState << endl;
          }

          des_vel /= 1.5;
        }

        return true;
      }

      /* determined or random inner point */
      if (!flag_randomPolyTraj)
      {
        if (innerPs.cols() != 0)
        {
          ROS_ERROR("innerPs.cols() != 0");
        }

        piece_nums = 1;
        piece_dur_vec.resize(1);
        piece_dur_vec(0) = init_of_init_totaldur;
      }
      else
      {
        Eigen::Vector3d horizen_dir = ((start_pt - local_target_pt).cross(Eigen::Vector3d(0, 0, 1))).normalized();
        Eigen::Vector3d vertical_dir = ((start_pt - local_target_pt).cross(horizen_dir)).normalized();
        innerPs.resize(3, 1);
        innerPs = (start_pt + local_target_pt) / 2 +
                  (((double)rand()) / RAND_MAX - 0.5) *
                      (start_pt - local_target_pt).norm() *
                      horizen_dir * 0.8 * (-0.978 / (continous_failures_count_ + 0.989) + 0.989) +
                  (((double)rand()) / RAND_MAX - 0.5) *
                      (start_pt - local_target_pt).norm() *
                      vertical_dir * 0.4 * (-0.978 / (continous_failures_count_ + 0.989) + 0.989);

        piece_nums = 2;
        piece_dur_vec.resize(2);
        piece_dur_vec = Eigen::Vector2d(init_of_init_totaldur / 2, init_of_init_totaldur / 2);
      }

      /* generate the init of init trajectory */
      initMJO.reset(headState, tailState, piece_nums);
      initMJO.generate(innerPs, piece_dur_vec);
      poly_traj::Trajectory initTraj = initMJO.getTraj();

      /* generate the real init trajectory */
      piece_nums = round((headState.col(0) - tailState.col(0)).norm() / pp_.polyTraj_piece_length);
      if (piece_nums < 2)
        piece_nums = 2;
      double piece_dur = init_of_init_totaldur / (double)piece_nums;
      piece_dur_vec.resize(piece_nums);
      piece_dur_vec = Eigen::VectorXd::Constant(piece_nums, ts);
      innerPs.resize(3, piece_nums - 1);
      int id = 0;
      double t_s = piece_dur, t_e = init_of_init_totaldur - piece_dur / 2;
      for (double t = t_s; t < t_e; t += piece_dur)
      {
        innerPs.col(id++) = initTraj.getPos(t);
      }
      if (id != piece_nums - 1)
      {
        ROS_ERROR("Should not happen! x_x");
        return false;
      }
      initMJO.reset(headState, tailState, piece_nums);
      initMJO.generate(innerPs, piece_dur_vec);
    }
    else /*** case 2: initialize from previous optimal trajectory ***/
    {
      ROS_ERROR("callReboundReplan(false, ...), flag_use_poly_init is false or have_new_target_ is false");
      // if (traj_.global_traj.last_glb_t_of_lc_tgt < 0.0)
      // {
      //   ROS_ERROR("You are initialzing a trajectory from a previous optimal trajectory, but no previous trajectories up to now.");
      //   return false;
      // }

      // /* the trajectory time system is a little bit complicated... */
      // double passed_t_on_lctraj = ros::Time::now().toSec() - traj_.local_traj.start_time;
      // double t_to_lc_end = traj_.local_traj.duration - passed_t_on_lctraj;
      // if (t_to_lc_end < 0)
      // {
      //   ROS_INFO("t_to_lc_end < 0, exit and wait for another call.");
      //   return false;
      // }
      // double t_to_lc_tgt = t_to_lc_end +
      //                      (traj_.global_traj.glb_t_of_lc_tgt - traj_.global_traj.last_glb_t_of_lc_tgt);
      // int piece_nums = ceil((start_pt - local_target_pt).norm() / pp_.polyTraj_piece_length);
      // if (piece_nums < 2)
      //   piece_nums = 2;

      // Eigen::Matrix3d headState, tailState;
      // Eigen::MatrixXd innerPs(3, piece_nums - 1);
      // Eigen::VectorXd piece_dur_vec = Eigen::VectorXd::Constant(piece_nums, t_to_lc_tgt / piece_nums);
      // headState << start_pt, start_vel, start_acc;
      // tailState << local_target_pt, local_target_vel, Eigen::Vector3d::Zero();

      // double t = piece_dur_vec(0);
      // for (int i = 0; i < piece_nums - 1; ++i)
      // {
      //   if (t < t_to_lc_end)
      //   {
      //     innerPs.col(i) = traj_.local_traj.traj.getPos(t + passed_t_on_lctraj);
      //   }
      //   else if (t <= t_to_lc_tgt)
      //   {
      //     double glb_t = t - t_to_lc_end + traj_.global_traj.last_glb_t_of_lc_tgt - traj_.global_traj.global_start_time;
      //     innerPs.col(i) = traj_.global_traj.traj.getPos(glb_t);
      //   }
      //   else
      //   {
      //     ROS_ERROR("Should not happen! x_x 0x88 t=%.2f, t_to_lc_end=%.2f, t_to_lc_tgt=%.2f", t, t_to_lc_end, t_to_lc_tgt);
      //   }

      //   t += piece_dur_vec(i + 1);
      // }

      // initMJO.reset(headState, tailState, piece_nums);
      // initMJO.generate(innerPs, piece_dur_vec);
    }

    return true;
  }

  void EGOPlannerManager::getLocalTarget(
      const double planning_horizen, const Eigen::Vector3d &start_pt,
      const Eigen::Vector3d &global_end_pt, Eigen::Vector3d &local_target_pos,
      Eigen::Vector3d &local_target_vel, bool &touch_goal)
  {
    double t;
    touch_goal = false;

    traj_.global_traj.last_glb_t_of_lc_tgt = traj_.global_traj.glb_t_of_lc_tgt;

    double t_step = planning_horizen / 20 / pp_.max_vel_;
    // double dist_min = 9999, dist_min_t = 0.0;
    for (t = traj_.global_traj.glb_t_of_lc_tgt;
         t < (traj_.global_traj.global_start_time + traj_.global_traj.duration);
         t += t_step)
    {
      Eigen::Vector3d pos_t = traj_.global_traj.traj.getPos(t - traj_.global_traj.global_start_time);
      double dist = (pos_t - start_pt).norm();

      if (dist >= planning_horizen)
      {
        local_target_pos = pos_t;
        traj_.global_traj.glb_t_of_lc_tgt = t;
        break;
      }
    }

    if ((t - traj_.global_traj.global_start_time) >= traj_.global_traj.duration - 1e-5) // Last global point
    {
      local_target_pos = global_end_pt;
      traj_.global_traj.glb_t_of_lc_tgt = traj_.global_traj.global_start_time + traj_.global_traj.duration;
      touch_goal = true;
    }

    if ((global_end_pt - local_target_pos).norm() < (pp_.max_vel_ * pp_.max_vel_) / (2 * pp_.max_acc_))
    {
      local_target_vel = Eigen::Vector3d::Zero();
    }
    else
    {
      local_target_vel = traj_.global_traj.traj.getVel(t - traj_.global_traj.global_start_time);
    }
  }

  bool EGOPlannerManager::setLocalTrajFromOpt(const poly_traj::MinJerkOpt &opt, const bool touch_goal)
  {
    poly_traj::Trajectory traj = opt.getTraj();
    Eigen::MatrixXd cps = opt.getInitConstraintPoints(getCpsNumPrePiece());
    PtsChk_t pts_to_check;
    bool ret = ploy_traj_opt_->computePointsToCheck(traj, ConstraintPoints::two_thirds_id(cps, touch_goal), pts_to_check);
    if (ret && pts_to_check.size() >= 1 && pts_to_check.back().size() >= 1)
    {
      traj_.setLocalTraj(traj, pts_to_check, ros::Time::now().toSec());
    }

    return ret;
  }

  bool EGOPlannerManager::EmergencyStop(Eigen::Vector3d stop_pos)
  {
    auto ZERO = Eigen::Vector3d::Zero();
    Eigen::Matrix<double, 3, 3> headState, tailState;
    headState << stop_pos, ZERO, ZERO;
    tailState = headState;
    poly_traj::MinJerkOpt stopMJO;
    stopMJO.reset(headState, tailState, 2);
    stopMJO.generate(stop_pos, Eigen::Vector2d(1.0, 1.0));

    setLocalTrajFromOpt(stopMJO, false);

    return true;
  }

  bool EGOPlannerManager::checkCollision(int drone_id)
  {
    if (traj_.local_traj.start_time < 1e9) // It means my first planning has not started
      return false;
    if (traj_.swarm_traj[drone_id].drone_id != drone_id) // The trajectory is invalid
      return false;

    double my_traj_start_time = traj_.local_traj.start_time;
    double other_traj_start_time = traj_.swarm_traj[drone_id].start_time;

    double t_start = max(my_traj_start_time, other_traj_start_time);
    double t_end = min(my_traj_start_time + traj_.local_traj.duration * 2 / 3,
                       other_traj_start_time + traj_.swarm_traj[drone_id].duration);

    for (double t = t_start; t < t_end; t += 0.03)
    {
      if ((traj_.local_traj.traj.getPos(t - my_traj_start_time) -
           traj_.swarm_traj[drone_id].traj.getPos(t - other_traj_start_time))
              .norm() < (getSwarmClearance() + traj_.swarm_traj[drone_id].des_clearance))
      {
        return true;
      }
    }

    return false;
  }

  bool EGOPlannerManager::planGlobalTrajWaypoints(
      const Eigen::Vector3d &start_pos, const Eigen::Vector3d &start_vel,
      const Eigen::Vector3d &start_acc, const std::vector<Eigen::Vector3d> &waypoints,
      const Eigen::Vector3d &end_vel, const Eigen::Vector3d &end_acc)
  {

    poly_traj::MinJerkOpt globalMJO;
    Eigen::Matrix<double, 3, 3> headState, tailState;
    headState << start_pos, start_vel, start_acc;
    tailState << waypoints.back(), end_vel, end_acc;
    Eigen::MatrixXd innerPts;

    if (waypoints.size() > 1)
    {

      innerPts.resize(3, waypoints.size() - 1);
      for (int i = 0; i < (int)waypoints.size() - 1; ++i)
      {
        innerPts.col(i) = waypoints[i];
      }
    }
    else
    {
      if (innerPts.size() != 0)
      {
        ROS_ERROR("innerPts.size() != 0");
      }
    }

    globalMJO.reset(headState, tailState, waypoints.size());

    double des_vel = pp_.max_vel_ / 1.5;
    Eigen::VectorXd time_vec(waypoints.size());

    for (int j = 0; j < 2; ++j)
    {
      for (size_t i = 0; i < waypoints.size(); ++i)
      {
        time_vec(i) = (i == 0) ? (waypoints[0] - start_pos).norm() / des_vel
                               : (waypoints[i] - waypoints[i - 1]).norm() / des_vel;
      }

      globalMJO.generate(innerPts, time_vec);

      if (globalMJO.getTraj().getMaxVelRate() < pp_.max_vel_ ||
          start_vel.norm() > pp_.max_vel_ ||
          end_vel.norm() > pp_.max_vel_)
      {
        break;
      }

      if (j == 2)
      {
        ROS_WARN("Global traj MaxVel = %f > set_max_vel", globalMJO.getTraj().getMaxVelRate());
        cout << "headState=" << endl
             << headState << endl;
        cout << "tailState=" << endl
             << tailState << endl;
      }

      des_vel /= 1.5;
    }

    auto time_now = ros::Time::now();
    traj_.setGlobalTraj(globalMJO.getTraj(), time_now.toSec());

    return true;
  }

} // namespace ego_planner
