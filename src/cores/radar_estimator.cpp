// This file is part of DeRO: Dead Reckoning Based on Radar Odometry
// With Accelerometers Aided for Robot Localization.
// Copyright (C) 2024 Hoang Viet Do  <hoangvietdo@sju.ac.kr>

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// Reference:
// https://github.com/christopherdoer/reve/blob/master/radar_ego_velocity_estimator/src/radar_ego_velocity_estimator.cpp

#include "dero/radar_estimator.hpp"

namespace incsl {

RadarEstimator::RadarEstimator() {} // RadarVelocityEstimator

bool RadarEstimator::Process(const sensor_msgs::msg::PointCloud2 &radar_PCL2_msg,
                             const RadarVelocityEstimatorParam   &param) {


  pcl::PointCloud<ar::StdPointRadar>::Ptr radar_pcl(new pcl::PointCloud<ar::StdPointRadar>);
  pcl::fromROSMsg(radar_PCL2_msg, *radar_pcl);
  
  for (auto & p: radar_pcl->points) {
    // Soft filtering
    if (p.range > param.min_distance && p.range < param.max_distance && p.signal_noise_ratio > param.min_db &&
        std::fabs(p.azimuth_angle) < param.azimuth_threshold * R2D && std::fabs(p.elevation_angle) < param.elevation_threshold * R2D) {
      
      if (p.z > param.filter_min_z && p.z < param.filter_max_z) {
        valid_targets_.push_back(p);
      } // if p_stab.z()
    }   // if r > param.min_distance
  }

  if (valid_targets_.size() > 2) {
    std::vector<double> v_dopplers;

    for (const auto &v : valid_targets_.points)
      v_dopplers.emplace_back(std::fabs(v.radial_speed));

    const size_t n = v_dopplers.size() * (1.0 - param.allowed_outlier_percentage);

    std::nth_element(v_dopplers.begin(), v_dopplers.begin() + n, v_dopplers.end());
    const auto median = v_dopplers[n];
    
    v_dopplers.clear();

    
    if (std::fabs(median) < param.zero_velocity_threshold) {
      zupt_trigger = true;
      radar_info_  = "Zero velocity detected!";

      v_r = Vec3d(0, 0, 0);
      P_v_r.setIdentity();
      P_v_r.diagonal() =
          Vec3d(param.sigma_zero_velocity_x, param.sigma_zero_velocity_y, param.sigma_zero_velocity_z).array().square();

      for (const auto &p : valid_targets_.points)
        if (std::fabs(p.radial_speed) < param.zero_velocity_threshold)
          radar_scan_inlier.push_back(p);

    } else {
      zupt_trigger = false;
      // LSQ velocity estimation
      radar_info_  = "No zero velocity!";
      MatXd radar_data(valid_targets_.size(), 4); // rx, ry, rz, v
      uint  j = 0;

      for (const auto &v : valid_targets_.points)
        radar_data.row(j++) = Vec4d(v.x, v.y, v.z, v.radial_speed);

      // RANSAC
      if (param.use_ransac) {
        std::vector<uint> inlier_idx_best;
        solve3DLsqRansac(radar_data, v_r, P_v_r, inlier_idx_best, param);

        for (const auto &idx : inlier_idx_best) {
          radar_scan_inlier.push_back(valid_targets_.at(idx));
          pcl_vec_.emplace_back(
              Vec3d(valid_targets_.points[idx].x, valid_targets_.points[idx].y, valid_targets_.points[idx].z));
        }

        if (param.use_odr && v_r.norm() > param.min_speed_odr && inlier_idx_best.size() > param.odr_inlier_threshold) {
          MatXd radar_data_inlier(inlier_idx_best.size(), 4);

          for (uint i = 0; i < inlier_idx_best.size(); ++i)
            radar_data_inlier.row(i) = radar_data.row(inlier_idx_best.at(i));

          std::cout << "using ODR" << std::endl;
          solve3DODR(radar_data_inlier, v_r, P_v_r, param);
        } // else std::cout << "ODR condition is not satisfied, skipping ODR algorithm!" << std::endl; // if use_odr
        inlier_idx_best.clear();
      } 
      else { 
        solve3DLsq(radar_data, v_r, P_v_r, true, param);

        for (const auto &item : valid_targets) {
          radar_scan_inlier.push_back(valid_targets_.at(idx));
          pcl_vec_.emplace_back(Vec3d(valid_targets_.points[idx].x, valid_targets_.points[idx].y, valid_targets_.points[idx].z));
        }

        if (param.use_odr && v_r.norm() > param.min_speed_odr && radar_scan_inlier.size() > param.odr_inlier_threshold)
          solve3DODR(radar_data, v_r, P_v_r, param);
      } // if-else use_ransac
    }   // if median
  
  }     // if valid_targets
  else {
    radar_info_  = "No valid target!";
  }

  setEgoVelocity(v_r);
  setEgoVelocityCovariance(P_v_r);

  radar_scan_inlier.height = 1;
  radar_scan_inlier.width  = radar_scan_inlier.size();

  pcl::toROSMsg(radar_scan_inlier, inlier_radar_msg_);
  inlier_radar_msg_.header = radar_PCL2_msg.header;

  setInlierRadarRos2PCL2(inlier_radar_msg_);
  setInlierRadarPcl(pcl_vec_);

  foo_radar_scan_inlier = radar_scan_inlier;
  
  pcl_vec_.clear();
  radar_scan_inlier.clear();
  valid_targets_.clear();

  if (zupt_trigger)
    return true;
  else
    return false;
} // Process

void RadarEstimator::setInlierRadarPcl(const std::vector<Vec3d> &pcl_vec) {
  inlier_pcl_vec = pcl_vec;
} // setInlierRadarPclPCL2

std::vector<Vec3d> RadarEstimator::getInlierRadarPcl() { return inlier_pcl_vec; } // getInlierRadarPcl

void RadarEstimator::setInlierRadarRos2PCL2(sensor_msgs::msg::PointCloud2 radar_msg) {
  inlier_radar_msg = radar_msg;
} // setInlierRadarRos2PCL2

sensor_msgs::msg::PointCloud2 RadarEstimator::getInlierRadarRos2PCL2() {
  return inlier_radar_msg;
} // getInlierRadarRos2PCL2

Vec3d RadarEstimator::getEgoVelocity() { return ego_velocity_; } // getEgoVelocity

void RadarEstimator::setEgoVelocity(Vec3d v_r) { ego_velocity_ = v_r; } // setEgoVelocity

Mat3d RadarEstimator::getEgoVelocityCovariance() { return ego_velocity_covariance_; } // getEgoVelocityCovariance

void RadarEstimator::setEgoVelocityCovariance(Mat3d P_v_r) {
  ego_velocity_covariance_ = P_v_r;
} // setEgoVelocityCovariance

std::string RadarEstimator::getRadarInfo() { return radar_info_; } // getRadarInfo

void RadarEstimator::solve3DLsqRansac(const MatXd &radar_data, Vec3d &v_r, Mat3d &P_v_r,
                                      std::vector<uint> &inlier_idx_best, const RadarVelocityEstimatorParam param) {
  MatXd H_all(radar_data.rows(), 3);
  H_all.col(0)      = radar_data.col(0);
  H_all.col(1)      = radar_data.col(1);
  H_all.col(2)      = radar_data.col(2);
  const VecXd y_all = radar_data.col(3);

  std::vector<uint> idx(radar_data.rows());
  for (uint k = 0; k < radar_data.rows(); ++k)
    idx[k] = k;

  std::random_device rd;
  std::mt19937       g(rd());

  ransac_iter_ = uint((std::log(1.0 - param.success_prob)) /
                      std::log(1.0 - std::pow(1.0 - param.outlier_prob, param.N_ransac_points)));

  for (uint k = 0; k < ransac_iter_; ++k) {
    std::shuffle(idx.begin(), idx.end(), g);
    MatXd radar_data_iter(param.N_ransac_points, 4);

    for (uint i = 0; i < param.N_ransac_points; ++i)
      radar_data_iter.row(i) = radar_data.row(idx.at(i));

    if (solve3DLsq(radar_data_iter, v_r, P_v_r, false, param)) {
      const VecXd err = (y_all - H_all * v_r).array().abs();

      std::vector<uint> inlier_idx;

      for (uint j = 0; j < err.rows(); ++j)
        if (err(j) < param.inlier_threshold)
          inlier_idx.emplace_back(j);

      if (inlier_idx.size() > inlier_idx_best.size())
        inlier_idx_best = inlier_idx;

      inlier_idx.clear();
    }
  }
  idx.clear();

  if (!inlier_idx_best.empty()) {
    MatXd radar_data_inlier(inlier_idx_best.size(), 4);
    for (uint i = 0; i < inlier_idx_best.size(); ++i)
      radar_data_inlier.row(i) = radar_data.row(inlier_idx_best.at(i));

    solve3DLsq(radar_data_inlier, v_r, P_v_r, true, param);
  }
} // solve3DLsqRansac

bool RadarEstimator::solve3DLsq(const MatXd &radar_data, Vec3d &v_r, Mat3d &P_v_r, bool estimate_sigma,
                                const RadarVelocityEstimatorParam param) {
  MatXd H(radar_data.rows(), 3);
  H.col(0)        = radar_data.col(0);
  H.col(1)        = radar_data.col(1);
  H.col(2)        = radar_data.col(2);
  const MatXd HTH = H.transpose() * H;

  const VecXd y = radar_data.col(3);

  Eigen::JacobiSVD<MatXd> svd(HTH);
  double                  cond = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);

  if (std::fabs(cond) < 1.0e3) {
    v_r = H.bdcSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(y);

    if (estimate_sigma) {

      const VecXd e   = H * v_r - y;
      P_v_r           = (e.transpose() * e).x() * (HTH).inverse() / (H.rows() - 3);
      Vec3d sigma_v_r = Vec3d(P_v_r(0, 0), P_v_r(1, 1), P_v_r(2, 2));

      const Vec3d offset =
          Vec3d(param.sigma_offset_radar_x, param.sigma_offset_radar_y, param.sigma_offset_radar_z).array().square();
      P_v_r += offset.asDiagonal();

      if (sigma_v_r.x() >= 0.0 && sigma_v_r.y() >= 0.0 && sigma_v_r.z() >= 0.0) {
        sigma_v_r = sigma_v_r.array().sqrt();

        if (sigma_v_r.x() < param.max_sigma_x && sigma_v_r.y() < param.max_sigma_y && sigma_v_r.z() < param.max_sigma_z)
          radar_info_ = "Valid LSQ-RANSAC estimation!";

        return true;
      } else {
        radar_info_ = "Invalid LSQ-RANSAC estimation!";
      }
    }
  } else {
    return true;
  }
  return false;
} // solve3DLsq

void RadarEstimator::solve3DODR(const MatXd &radar_data, Vec3d &v_r, Mat3d &P_v_r,
                                const RadarVelocityEstimatorParam param_) {
  const auto n = radar_data.rows();

  // radar_data: rx, ry, rz, v
  Eigen::MatrixXd H(n, 3);
  H.col(0) = radar_data.col(0);
  H.col(1) = radar_data.col(1);
  H.col(2) = radar_data.col(2);

  Eigen::VectorXd y(n);
  y = radar_data.col(3);

  Eigen::VectorXd sigma_y(n);
  sigma_y = param_.sigma_v_d * Eigen::VectorXd::Ones(n);

  Eigen::MatrixXd sigma_x(n, 3);

  for (uint k = 0; k < n; ++k) {
    sigma_x(k, 0) = (param_.model_noise_offset_deg) + (param_.model_noise_scale_deg) * (1.0 - std::fabs(H(k, 0)));
    sigma_x(k, 1) = (param_.model_noise_offset_deg) + (param_.model_noise_scale_deg) * (1.0 - std::fabs(H(k, 1)));
    sigma_x(k, 2) = (param_.model_noise_offset_deg) + (param_.model_noise_scale_deg) * (1.0 - std::fabs(H(k, 2)));
  }

  Eigen::VectorXd v_r_odr(3);
  v_r_odr = v_r;

  Eigen::VectorXd sigma_odr(3);
  Eigen::MatrixXd cov_v_r(3, 3);

  incsl::solveODR(y, H, sigma_y, sigma_x, v_r_odr, sigma_odr, cov_v_r);

  v_r   = v_r_odr;
  P_v_r = cov_v_r;
  const Vec3d offset =
      Vec3d(param_.sigma_offset_radar_x, param_.sigma_offset_radar_y, param_.sigma_offset_radar_z).array().square();

  P_v_r += offset.asDiagonal();

  if (sigma_odr.x() < param_.max_sigma_x && sigma_odr.y() < param_.max_sigma_y && sigma_odr.z() < param_.max_sigma_z)
    std::cout << "ODR: Success" << std::endl;
  else
    std::cout << "ODR: Failed" << std::endl;
} // solve3DODR

ICPTransform RadarEstimator::solveICP(const pcl::PointCloud<ar::StdPointRadar> &prev_pcl_msg,
                                      const pcl::PointCloud<ar::StdPointRadar> &curr_pcl_msg,
                                      const RadarPositionEstimatorParam                 &radar_position_estimator_param,
                                      const Mat4d                                       &init_guess_pose) {

  pcl::PointCloud<pcl::PointXYZ>::Ptr prev_(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::PointXYZ>::Ptr curr_(new pcl::PointCloud<pcl::PointXYZ>);

  prev_->points.resize(prev_pcl_msg.size());
  curr_->points.resize(curr_pcl_msg.size());

  const bool normalization = false;

  pcl::PointCloud<ar::StdPointRadar> prev_pcl_normalized_msg = prev_pcl_msg;
  pcl::PointCloud<ar::StdPointRadar> curr_pcl_normalized_msg = curr_pcl_msg;

  if (normalization) {
    pcl::PointCloud<ar::StdPointRadar> prev_pcl_normalized_msg = normalizedPointCloud(prev_pcl_msg);
    pcl::PointCloud<ar::StdPointRadar> curr_pcl_normalized_msg = normalizedPointCloud(curr_pcl_msg);
  }

  for (std::size_t i = 0; i < prev_pcl_normalized_msg.points.size(); ++i) {
    prev_->at(i).x = prev_pcl_normalized_msg.points[i].x;
    prev_->at(i).y = prev_pcl_normalized_msg.points[i].y;
    prev_->at(i).z = prev_pcl_normalized_msg.points[i].z;
  }

  for (std::size_t i = 0; i < curr_pcl_normalized_msg.points.size(); ++i) {
    curr_->at(i).x = curr_pcl_normalized_msg.points[i].x;
    curr_->at(i).y = curr_pcl_normalized_msg.points[i].y;
    curr_->at(i).z = curr_pcl_normalized_msg.points[i].z;
  }


  prev_->width = prev_->points.size();
  prev_->height = 1;
  curr_->width = curr_->points.size();
  curr_->height = 1;

  pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;

  if (radar_position_estimator_param.max_corres_dis != 0.0)
    icp.setMaxCorrespondenceDistance(radar_position_estimator_param.max_corres_dis);

  if (radar_position_estimator_param.transform_eps != 0.0)
    icp.setTransformationEpsilon(radar_position_estimator_param.transform_eps);

  if (radar_position_estimator_param.max_iter != 0)
    icp.setMaximumIterations(radar_position_estimator_param.max_iter);

  if (radar_position_estimator_param.euclidean_fit_eps != 0.0)
    icp.setEuclideanFitnessEpsilon(radar_position_estimator_param.euclidean_fit_eps);

  if (radar_position_estimator_param.ransac_outlier_reject_threshold != 0.0)
    icp.setRANSACOutlierRejectionThreshold(radar_position_estimator_param.ransac_outlier_reject_threshold);

  pcl::PointCloud<pcl::PointXYZ>::Ptr align(new pcl::PointCloud<pcl::PointXYZ>);
  icp.setInputSource(prev_);
  icp.setInputTarget(curr_);

  const Mat4f guess_pose = init_guess_pose.cast<float>();

  icp.align(*align, guess_pose);

  const Mat4f  prev2curr = icp.getFinalTransformation();
  const double score     = icp.getFitnessScore();

  const Mat4d prev2curr_       = prev2curr.cast<double>();
  Mat4d       curr2prev_       = Mat4d::Identity();
  curr2prev_.block<3, 3>(0, 0) = prev2curr_.block<3, 3>(0, 0).transpose();
  curr2prev_.block<3, 1>(0, 3) = -prev2curr_.block<3, 3>(0, 0).transpose() * prev2curr_.block<3, 1>(0, 3);

  ICPTransform icp_results;
  icp_results.rotation    = Mat3d::Identity();
  icp_results.translation = Vec3d::Zero();
  icp_results.homo        = Mat4d::Identity();
  icp_results.score       = 0.0;

  icp_results.rotation               = curr2prev_.block<3, 3>(0, 0);
  icp_results.translation            = curr2prev_.block<3, 1>(0, 3);
  icp_results.homo.block<3, 3>(0, 0) = icp_results.rotation;
  icp_results.homo.block<3, 1>(0, 3) = icp_results.translation;
  icp_results.score                  = score;
  icp_results.number_of_points       = curr_pcl_normalized_msg.points.size();

  bool is_converged = icp.hasConverged();
  if (!is_converged) {
    std::cout << "ICP is not converged!";
  }

  pcl::CorrespondencesPtr corresps(new pcl::Correspondences);

  pcl::registration::CorrespondenceEstimation<pcl::PointXYZ, pcl::PointXYZ> corresp_est;
  corresp_est.setInputSource(align);
  corresp_est.setInputTarget(curr_);
  corresp_est.determineCorrespondences(*corresps, radar_position_estimator_param.max_corres_dis);

  double sum_x        = 0.0;
  double sum_y        = 0.0;
  double sum_z        = 0.0;
  double m_sqrt_sum_x = 0.0;
  double m_sqrt_sum_y = 0.0;
  double m_sqrt_sum_z = 0.0;

  for (std::size_t i = 0; i < corresps->size(); ++i) {
    const int idx_source = corresps->at(i).index_query;
    const int idx_target = corresps->at(i).index_match;

    const double foo_x  = curr_->at(idx_target).x - align->at(idx_source).x;
    const double foo_y  = curr_->at(idx_target).y - align->at(idx_source).y;
    const double foo_z  = curr_->at(idx_target).z - align->at(idx_source).z;
    sum_x              += (foo_x * foo_x);
    sum_y              += (foo_y * foo_y);
    sum_z              += (foo_z * foo_z);
  }

  // const double N = static_cast<double>(corresps->size());
  m_sqrt_sum_x = std::sqrt(sum_x) / corresps->size();
  m_sqrt_sum_y = std::sqrt(sum_y) / corresps->size();
  m_sqrt_sum_z = std::sqrt(sum_z) / corresps->size();

  // const double sum  = sum_x + sum_y + sum_z;
  icp_results.P_vec = Vec3d::Zero();

  // clang-format off
  icp_results.P_vec << radar_position_estimator_param.icp_std_x + icp_results.score * m_sqrt_sum_x,
                       radar_position_estimator_param.icp_std_y + icp_results.score * m_sqrt_sum_y,
                       radar_position_estimator_param.icp_std_z + icp_results.score * m_sqrt_sum_z;
  // clang-format on

  //  NOTE: this transform is for "only radar localization"
  const bool radarOdom = false;
  if (radarOdom) {
    Mat4d curr_trans = prev_trans * prev2curr_;
    prev_trans       = curr_trans;

    trans.rotation               = Mat3d::Identity();
    trans.translation            = Vec3d::Zero();
    trans.homo                   = Mat4d::Identity();
    trans.score                  = 0.0;
    trans.rotation               = curr_trans.block<3, 3>(0, 0);
    trans.translation            = curr_trans.block<3, 1>(0, 3);
    trans.homo.block<3, 3>(0, 0) = trans.rotation;
    trans.homo.block<3, 1>(0, 3) = trans.translation;
    trans.score                  = score;
    trans.number_of_points       = curr_pcl_normalized_msg.points.size();
  }
  return icp_results;
} // solveICP

void RadarEstimator::Colorize(const pcl::PointCloud<pcl::PointXYZ> &pc, pcl::PointCloud<pcl::PointXYZRGB> &pc_colored,
                              const std::vector<int> &color) {

  int N = pc.points.size();

  pc_colored.clear();
  pcl::PointXYZRGB pt_tmp;
  for (int i = 0; i < N; ++i) {
    const auto &pt = pc.points[i];
    pt_tmp.x       = pt.x;
    pt_tmp.y       = pt.y;
    pt_tmp.z       = pt.z;
    pt_tmp.r       = color[0];
    pt_tmp.g       = color[1];
    pt_tmp.b       = color[2];
    pc_colored.points.emplace_back(pt_tmp);
  }
} // void Colorize

pcl::PointCloud<ar::StdPointRadar>
RadarEstimator::normalizedPointCloud(const pcl::PointCloud<ar::StdPointRadar> &raw_pcl_msg) {

  std::vector<double> buffer_x, buffer_y, buffer_z;
  for (std::size_t i = 0; i < raw_pcl_msg.points.size(); ++i) {
    buffer_x.push_back(raw_pcl_msg.points[i].x);
    buffer_y.push_back(raw_pcl_msg.points[i].y);
    buffer_z.push_back(raw_pcl_msg.points[i].z);
  }

  const double max_x = *std::max_element(buffer_x.begin(), buffer_x.end());
  const double min_x = *std::min_element(buffer_x.begin(), buffer_x.end());

  const double max_y = *std::max_element(buffer_y.begin(), buffer_y.end());
  const double min_y = *std::min_element(buffer_y.begin(), buffer_y.end());

  const double max_z = *std::max_element(buffer_z.begin(), buffer_z.end());
  const double min_z = *std::min_element(buffer_z.begin(), buffer_z.end());

  const double d_x = max_x - min_x;
  const double d_y = max_y - min_y;
  const double d_z = max_z - min_z;

  pcl::PointCloud<ar::StdPointRadar> normalized_pcl_msg;
  normalized_pcl_msg = raw_pcl_msg;

  for (std::size_t i = 0; i < raw_pcl_msg.points.size(); ++i) {
    normalized_pcl_msg.points[i].x = (raw_pcl_msg.points[i].x - min_x) / std::abs(d_x);
    normalized_pcl_msg.points[i].y = (raw_pcl_msg.points[i].y - min_y) / std::abs(d_y);
    normalized_pcl_msg.points[i].z = (raw_pcl_msg.points[i].z - min_z) / std::abs(d_z);
  }

  return normalized_pcl_msg;
} // normalizedPointCloud

ICPTransform RadarEstimator::getIcpTransform() { return trans; } // getICPTransform

RadarPointCloudType RadarEstimator::toRadarPointCloudType(const Vec11d &item, const RadarIndex &idx) {
  RadarPointCloudType point;
  point.x             = item[idx.x_r];
  point.y             = item[idx.y_r];
  point.z             = item[idx.z_r];
  point.v_doppler_mps = -item[idx.v_d];
  point.snr_db        = item[idx.peak_db];
  point.noise_db      = item[idx.noise_db];
  return point;
} // toRadarPointCloudType

pcl::PointCloud<ar::StdPointRadar> RadarEstimator::getRadarScanInlier() {
  return foo_radar_scan_inlier;
} // getRadarScanInlier

} // namespace incsl
