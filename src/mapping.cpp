// This is an advanced implementation of the algorithm described in the following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014. 

// Modifier: Tong Qin               qintonguav@gmail.com
// 	         Shaozu Cao 		    saozu.cao@connect.ust.hk


// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <math.h>
#include <vector>
#include "mloam/mloam.h"
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <eigen3/Eigen/Dense>
#include <ceres/ceres.h>
#include <mutex>
#include <queue>
#include <thread>
#include <iostream>
#include <string>

#include "mloam/lidar_factor.h"


static int frameCount = 0;

static double timeLaserCloudCornerLast = 0;
static double timeLaserCloudSurfLast = 0;
static double timeLaserCloudFullRes = 0;
static double timeLaserOdometry = 0;


static int laserCloudCenWidth = 10;
static int laserCloudCenHeight = 10;
static int laserCloudCenDepth = 5;
static const int laserCloudWidth = 21;
static const int laserCloudHeight = 21;
static const int laserCloudDepth = 11;


static const int laserCloudNum = laserCloudWidth * laserCloudHeight * laserCloudDepth; //4851


static int laserCloudValidInd[125];
static int laserCloudSurroundInd[125];

// input: from odom
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCornerLast(new pcl::PointCloud<pcl::PointXYZI>());
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudSurfLast(new pcl::PointCloud<pcl::PointXYZI>());
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudFullRes(new pcl::PointCloud<pcl::PointXYZI>());

// ouput: all visualble cube points
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudSurround(new pcl::PointCloud<pcl::PointXYZI>());

// surround points in map to build tree
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCornerFromMap(new pcl::PointCloud<pcl::PointXYZI>());
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudSurfFromMap(new pcl::PointCloud<pcl::PointXYZI>());

//input & output: points in one frame. local --> global

// points in every cube
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCornerArray[laserCloudNum];
static pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudSurfArray[laserCloudNum];

//kd-tree
static pcl::KdTreeFLANN<pcl::PointXYZI>::Ptr kdtreeCornerFromMap(new pcl::KdTreeFLANN<pcl::PointXYZI>());
static pcl::KdTreeFLANN<pcl::PointXYZI>::Ptr kdtreeSurfFromMap(new pcl::KdTreeFLANN<pcl::PointXYZI>());

static double parameters[7] = {0, 0, 0, 1, 0, 0, 0};
static Eigen::Map<Eigen::Quaterniond> q_w_curr(parameters);
static Eigen::Map<Eigen::Vector3d> t_w_curr(parameters + 4);

// wmap_T_odom * odom_T_curr = wmap_T_curr;
// transformation between odom's world and map's world frame
static Eigen::Quaterniond q_wmap_wodom(1, 0, 0, 0);
static Eigen::Vector3d t_wmap_wodom(0, 0, 0);

static Eigen::Quaterniond q_wodom_curr(1, 0, 0, 0);
static Eigen::Vector3d t_wodom_curr(0, 0, 0);

static mloam::OdometryData odomAftMapped;

static pcl::VoxelGrid<pcl::PointXYZI> downSizeFilterCorner;
static pcl::VoxelGrid<pcl::PointXYZI> downSizeFilterSurf;

static std::vector<int> pointSearchInd;
static std::vector<float> pointSearchSqDis;

static pcl::PointXYZI pointOri, pointSel;

// set initial guess
void transformAssociateToMap()
{
	q_w_curr = q_wmap_wodom * q_wodom_curr;
	t_w_curr = q_wmap_wodom * t_wodom_curr + t_wmap_wodom;
}

void transformUpdate()
{
	q_wmap_wodom = q_w_curr * q_wodom_curr.inverse();
	t_wmap_wodom = t_w_curr - q_wmap_wodom * t_wodom_curr;
}

void pointAssociateToMap(pcl::PointXYZI const *const pi, pcl::PointXYZI *const po)
{
	Eigen::Vector3d point_curr(pi->x, pi->y, pi->z);
	Eigen::Vector3d point_w = q_w_curr * point_curr + t_w_curr;
	po->x = point_w.x();
	po->y = point_w.y();
	po->z = point_w.z();
	po->intensity = pi->intensity;
	//po->intensity = 1.0;
}

void pointAssociateTobeMapped(pcl::PointXYZI const *const pi, pcl::PointXYZI *const po)
{
	Eigen::Vector3d point_w(pi->x, pi->y, pi->z);
	Eigen::Vector3d point_curr = q_w_curr.inverse() * (point_w - t_w_curr);
	po->x = point_curr.x();
	po->y = point_curr.y();
	po->z = point_curr.z();
	po->intensity = pi->intensity;
}

void laserOdometryHandler(const mloam::OdometryData &laserOdometry) {

	// This function takes incoming laser odometry from the odometry section, and transforms it
	// into the map/world frame using the most recent world transformation data and then 
	// originally publishes it on a ros topic.
	// Here we are just setting a global odomAftMapped and will deal with it later.

	Eigen::Quaterniond q_wodom_curr;
	Eigen::Vector3d t_wodom_curr;
	q_wodom_curr.x() = laserOdometry.pose.pose.orientation.x();
	q_wodom_curr.y() = laserOdometry.pose.pose.orientation.y();
	q_wodom_curr.z() = laserOdometry.pose.pose.orientation.z();
	q_wodom_curr.w() = laserOdometry.pose.pose.orientation.w();
	t_wodom_curr.x() = laserOdometry.pose.pose.position.x();
	t_wodom_curr.y() = laserOdometry.pose.pose.position.y();
	t_wodom_curr.z() = laserOdometry.pose.pose.position.z();

	Eigen::Quaterniond q_w_curr = q_wmap_wodom * q_wodom_curr;
	Eigen::Vector3d t_w_curr = q_wmap_wodom * t_wodom_curr + t_wmap_wodom; 

	odomAftMapped.pose.pose.orientation.x() = q_w_curr.x();
	odomAftMapped.pose.pose.orientation.y() = q_w_curr.y();
	odomAftMapped.pose.pose.orientation.z() = q_w_curr.z();
	odomAftMapped.pose.pose.orientation.w() = q_w_curr.w();
	odomAftMapped.pose.pose.position.x() = t_w_curr.x();
	odomAftMapped.pose.pose.position.y() = t_w_curr.y();
	odomAftMapped.pose.pose.position.z() = t_w_curr.z();

	std::cout << "2," << odomAftMapped.pose.pose.position.x() << ","
		<< odomAftMapped.pose.pose.position.y() << ","
		<< odomAftMapped.pose.pose.position.z() << std::endl;
}

void mloam::Mapping(const pcl::PointCloud<pcl::PointXYZI>& lastCloudCornerLast,
									  const pcl::PointCloud<pcl::PointXYZI>& laserCloudSurfaceLast,
									  const pcl::PointCloud<pcl::PointXYZI>& laserCloudFullResolution,
			              const mloam::OdometryData& laserOdomToInit) {
	
	static bool do_once = true;
	if (do_once) {
		static const float lineRes = 0.4;
		static const float planeRes = 0.8;
		printf("line resolution %f plane resolution %f \n", lineRes, planeRes);
		downSizeFilterCorner.setLeafSize(lineRes, lineRes,lineRes);
		downSizeFilterSurf.setLeafSize(planeRes, planeRes, planeRes);
		for (int i = 0; i < laserCloudNum; i++)
		{
			laserCloudCornerArray[i].reset(new pcl::PointCloud<pcl::PointXYZI>());
			laserCloudSurfArray[i].reset(new pcl::PointCloud<pcl::PointXYZI>());
		}
		do_once = false;
	}

	laserCloudCornerLast = lastCloudCornerLast.makeShared();
	laserCloudSurfLast = laserCloudSurfaceLast.makeShared();
	laserCloudFullRes = laserCloudFullResolution.makeShared();

	// Initially this was a callback/ros subscriber but moving it to here
	laserOdometryHandler(laserOdomToInit);
	// while (1) {
		
		q_wodom_curr.x() = laserOdomToInit.pose.pose.orientation.x();
		q_wodom_curr.y() = laserOdomToInit.pose.pose.orientation.y();
		q_wodom_curr.z() = laserOdomToInit.pose.pose.orientation.z();
		q_wodom_curr.w() = laserOdomToInit.pose.pose.orientation.w();
		t_wodom_curr.x() = laserOdomToInit.pose.pose.position.x();
		t_wodom_curr.y() = laserOdomToInit.pose.pose.position.y();
		t_wodom_curr.z() = laserOdomToInit.pose.pose.position.z();

		transformAssociateToMap();

		int centerCubeI = int((t_w_curr.x() + 25.0) / 50.0) + laserCloudCenWidth;
		int centerCubeJ = int((t_w_curr.y() + 25.0) / 50.0) + laserCloudCenHeight;
		int centerCubeK = int((t_w_curr.z() + 25.0) / 50.0) + laserCloudCenDepth;

		if (t_w_curr.x() + 25.0 < 0)
			centerCubeI--;
		if (t_w_curr.y() + 25.0 < 0)
			centerCubeJ--;
		if (t_w_curr.z() + 25.0 < 0)
			centerCubeK--;

		while (centerCubeI < 3)
		{
			for (int j = 0; j < laserCloudHeight; j++)
			{
				for (int k = 0; k < laserCloudDepth; k++)
				{ 
					int i = laserCloudWidth - 1;
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeCornerPointer =
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k]; 
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeSurfPointer =
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					for (; i >= 1; i--)
					{
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudCornerArray[i - 1 + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudSurfArray[i - 1 + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					}
					laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeCornerPointer;
					laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeSurfPointer;
					laserCloudCubeCornerPointer->clear();
					laserCloudCubeSurfPointer->clear();
				}
			}

			centerCubeI++;
			laserCloudCenWidth++;
		}

		while (centerCubeI >= laserCloudWidth - 3)
		{ 
			for (int j = 0; j < laserCloudHeight; j++)
			{
				for (int k = 0; k < laserCloudDepth; k++)
				{
					int i = 0;
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeCornerPointer =
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeSurfPointer =
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					for (; i < laserCloudWidth - 1; i++)
					{
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudCornerArray[i + 1 + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudSurfArray[i + 1 + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					}
					laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeCornerPointer;
					laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeSurfPointer;
					laserCloudCubeCornerPointer->clear();
					laserCloudCubeSurfPointer->clear();
				}
			}

			centerCubeI--;
			laserCloudCenWidth--;
		}

		while (centerCubeJ < 3)
		{
			for (int i = 0; i < laserCloudWidth; i++)
			{
				for (int k = 0; k < laserCloudDepth; k++)
				{
					int j = laserCloudHeight - 1;
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeCornerPointer =
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeSurfPointer =
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					for (; j >= 1; j--)
					{
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudCornerArray[i + laserCloudWidth * (j - 1) + laserCloudWidth * laserCloudHeight * k];
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudSurfArray[i + laserCloudWidth * (j - 1) + laserCloudWidth * laserCloudHeight * k];
					}
					laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeCornerPointer;
					laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeSurfPointer;
					laserCloudCubeCornerPointer->clear();
					laserCloudCubeSurfPointer->clear();
				}
			}

			centerCubeJ++;
			laserCloudCenHeight++;
		}

		while (centerCubeJ >= laserCloudHeight - 3)
		{
			for (int i = 0; i < laserCloudWidth; i++)
			{
				for (int k = 0; k < laserCloudDepth; k++)
				{
					int j = 0;
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeCornerPointer =
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeSurfPointer =
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					for (; j < laserCloudHeight - 1; j++)
					{
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudCornerArray[i + laserCloudWidth * (j + 1) + laserCloudWidth * laserCloudHeight * k];
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudSurfArray[i + laserCloudWidth * (j + 1) + laserCloudWidth * laserCloudHeight * k];
					}
					laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeCornerPointer;
					laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeSurfPointer;
					laserCloudCubeCornerPointer->clear();
					laserCloudCubeSurfPointer->clear();
				}
			}

			centerCubeJ--;
			laserCloudCenHeight--;
		}

		while (centerCubeK < 3)
		{
			for (int i = 0; i < laserCloudWidth; i++)
			{
				for (int j = 0; j < laserCloudHeight; j++)
				{
					int k = laserCloudDepth - 1;
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeCornerPointer =
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeSurfPointer =
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					for (; k >= 1; k--)
					{
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * (k - 1)];
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * (k - 1)];
					}
					laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeCornerPointer;
					laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeSurfPointer;
					laserCloudCubeCornerPointer->clear();
					laserCloudCubeSurfPointer->clear();
				}
			}

			centerCubeK++;
			laserCloudCenDepth++;
		}

		while (centerCubeK >= laserCloudDepth - 3)
		{
			for (int i = 0; i < laserCloudWidth; i++)
			{
				for (int j = 0; j < laserCloudHeight; j++)
				{
					int k = 0;
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeCornerPointer =
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCubeSurfPointer =
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
					for (; k < laserCloudDepth - 1; k++)
					{
						laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * (k + 1)];
						laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
							laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * (k + 1)];
					}
					laserCloudCornerArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeCornerPointer;
					laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
						laserCloudCubeSurfPointer;
					laserCloudCubeCornerPointer->clear();
					laserCloudCubeSurfPointer->clear();
				}
			}

			centerCubeK--;
			laserCloudCenDepth--;
		}

		int laserCloudValidNum = 0;
		int laserCloudSurroundNum = 0;

		for (int i = centerCubeI - 2; i <= centerCubeI + 2; i++)
		{
			for (int j = centerCubeJ - 2; j <= centerCubeJ + 2; j++)
			{
				for (int k = centerCubeK - 1; k <= centerCubeK + 1; k++)
				{
					if (i >= 0 && i < laserCloudWidth &&
						j >= 0 && j < laserCloudHeight &&
						k >= 0 && k < laserCloudDepth)
					{ 
						laserCloudValidInd[laserCloudValidNum] = i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k;
						laserCloudValidNum++;
						laserCloudSurroundInd[laserCloudSurroundNum] = i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k;
						laserCloudSurroundNum++;
					}
				}
			}
		}

		laserCloudCornerFromMap->clear();
		laserCloudSurfFromMap->clear();
		for (int i = 0; i < laserCloudValidNum; i++)
		{
			*laserCloudCornerFromMap += *laserCloudCornerArray[laserCloudValidInd[i]];
			*laserCloudSurfFromMap += *laserCloudSurfArray[laserCloudValidInd[i]];
		}
		int laserCloudCornerFromMapNum = laserCloudCornerFromMap->points.size();
		int laserCloudSurfFromMapNum = laserCloudSurfFromMap->points.size();


		pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudCornerStack(new pcl::PointCloud<pcl::PointXYZI>());
		downSizeFilterCorner.setInputCloud(laserCloudCornerLast);
		downSizeFilterCorner.filter(*laserCloudCornerStack);
		int laserCloudCornerStackNum = laserCloudCornerStack->points.size();

		pcl::PointCloud<pcl::PointXYZI>::Ptr laserCloudSurfStack(new pcl::PointCloud<pcl::PointXYZI>());
		downSizeFilterSurf.setInputCloud(laserCloudSurfLast);
		downSizeFilterSurf.filter(*laserCloudSurfStack);
		int laserCloudSurfStackNum = laserCloudSurfStack->points.size();

		// printf("map corner num %d  surf num %d \n", laserCloudCornerFromMapNum, laserCloudSurfFromMapNum);
		if (laserCloudCornerFromMapNum > 10 && laserCloudSurfFromMapNum > 50)
		{
			kdtreeCornerFromMap->setInputCloud(laserCloudCornerFromMap);
			kdtreeSurfFromMap->setInputCloud(laserCloudSurfFromMap);

			for (int iterCount = 0; iterCount < 2; iterCount++)
			{
				//ceres::LossFunction *loss_function = NULL;
				ceres::LossFunction *loss_function = new ceres::HuberLoss(0.1);
				ceres::LocalParameterization *q_parameterization =
					new ceres::EigenQuaternionParameterization();
				ceres::Problem::Options problem_options;

				ceres::Problem problem(problem_options);
				problem.AddParameterBlock(parameters, 4, q_parameterization);
				problem.AddParameterBlock(parameters + 4, 3);

				int corner_num = 0;

				for (int i = 0; i < laserCloudCornerStackNum; i++)
				{
					pointOri = laserCloudCornerStack->points[i];
					//double sqrtDis = pointOri.x * pointOri.x + pointOri.y * pointOri.y + pointOri.z * pointOri.z;
					pointAssociateToMap(&pointOri, &pointSel);
					kdtreeCornerFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis); 

					if (pointSearchSqDis[4] < 1.0)
					{ 
						std::vector<Eigen::Vector3d> nearCorners;
						Eigen::Vector3d center(0, 0, 0);
						for (int j = 0; j < 5; j++)
						{
							Eigen::Vector3d tmp(laserCloudCornerFromMap->points[pointSearchInd[j]].x,
												laserCloudCornerFromMap->points[pointSearchInd[j]].y,
												laserCloudCornerFromMap->points[pointSearchInd[j]].z);
							center = center + tmp;
							nearCorners.push_back(tmp);
						}
						center = center / 5.0;

						Eigen::Matrix3d covMat = Eigen::Matrix3d::Zero();
						for (int j = 0; j < 5; j++)
						{
							Eigen::Matrix<double, 3, 1> tmpZeroMean = nearCorners[j] - center;
							covMat = covMat + tmpZeroMean * tmpZeroMean.transpose();
						}

						Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> saes(covMat);

						// if is indeed line feature
						// note Eigen library sort eigenvalues in increasing order
						Eigen::Vector3d unit_direction = saes.eigenvectors().col(2);
						Eigen::Vector3d curr_point(pointOri.x, pointOri.y, pointOri.z);
						if (saes.eigenvalues()[2] > 3 * saes.eigenvalues()[1])
						{ 
							Eigen::Vector3d point_on_line = center;
							Eigen::Vector3d point_a, point_b;
							point_a = 0.1 * unit_direction + point_on_line;
							point_b = -0.1 * unit_direction + point_on_line;

							ceres::CostFunction *cost_function = LidarEdgeFactor::Create(curr_point, point_a, point_b, 1.0);
							problem.AddResidualBlock(cost_function, loss_function, parameters, parameters + 4);
							corner_num++;	
						}							
					}
					/*
					else if(pointSearchSqDis[4] < 0.01 * sqrtDis)
					{
						Eigen::Vector3d center(0, 0, 0);
						for (int j = 0; j < 5; j++)
						{
							Eigen::Vector3d tmp(laserCloudCornerFromMap->points[pointSearchInd[j]].x,
												laserCloudCornerFromMap->points[pointSearchInd[j]].y,
												laserCloudCornerFromMap->points[pointSearchInd[j]].z);
							center = center + tmp;
						}
						center = center / 5.0;	
						Eigen::Vector3d curr_point(pointOri.x, pointOri.y, pointOri.z);
						ceres::CostFunction *cost_function = LidarDistanceFactor::Create(curr_point, center);
						problem.AddResidualBlock(cost_function, loss_function, parameters, parameters + 4);
					}
					*/
				}

				int surf_num = 0;
				for (int i = 0; i < laserCloudSurfStackNum; i++)
				{
					pointOri = laserCloudSurfStack->points[i];
					//double sqrtDis = pointOri.x * pointOri.x + pointOri.y * pointOri.y + pointOri.z * pointOri.z;
					pointAssociateToMap(&pointOri, &pointSel);
					kdtreeSurfFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

					Eigen::Matrix<double, 5, 3> matA0;
					Eigen::Matrix<double, 5, 1> matB0 = -1 * Eigen::Matrix<double, 5, 1>::Ones();
					if (pointSearchSqDis[4] < 1.0)
					{
						
						for (int j = 0; j < 5; j++)
						{
							matA0(j, 0) = laserCloudSurfFromMap->points[pointSearchInd[j]].x;
							matA0(j, 1) = laserCloudSurfFromMap->points[pointSearchInd[j]].y;
							matA0(j, 2) = laserCloudSurfFromMap->points[pointSearchInd[j]].z;
							//printf(" pts %f %f %f ", matA0(j, 0), matA0(j, 1), matA0(j, 2));
						}
						// find the norm of plane
						Eigen::Vector3d norm = matA0.colPivHouseholderQr().solve(matB0);
						double negative_OA_dot_norm = 1 / norm.norm();
						norm.normalize();

						// Here n(pa, pb, pc) is unit norm of plane
						bool planeValid = true;
						for (int j = 0; j < 5; j++)
						{
							// if OX * n > 0.2, then plane is not fit well
							if (fabs(norm(0) * laserCloudSurfFromMap->points[pointSearchInd[j]].x +
									 norm(1) * laserCloudSurfFromMap->points[pointSearchInd[j]].y +
									 norm(2) * laserCloudSurfFromMap->points[pointSearchInd[j]].z + negative_OA_dot_norm) > 0.2)
							{
								planeValid = false;
								break;
							}
						}
						Eigen::Vector3d curr_point(pointOri.x, pointOri.y, pointOri.z);
						if (planeValid)
						{
							ceres::CostFunction *cost_function = LidarPlaneNormFactor::Create(curr_point, norm, negative_OA_dot_norm);
							problem.AddResidualBlock(cost_function, loss_function, parameters, parameters + 4);
							surf_num++;
						}
					}
					/*
					else if(pointSearchSqDis[4] < 0.01 * sqrtDis)
					{
						Eigen::Vector3d center(0, 0, 0);
						for (int j = 0; j < 5; j++)
						{
							Eigen::Vector3d tmp(laserCloudSurfFromMap->points[pointSearchInd[j]].x,
												laserCloudSurfFromMap->points[pointSearchInd[j]].y,
												laserCloudSurfFromMap->points[pointSearchInd[j]].z);
							center = center + tmp;
						}
						center = center / 5.0;	
						Eigen::Vector3d curr_point(pointOri.x, pointOri.y, pointOri.z);
						ceres::CostFunction *cost_function = LidarDistanceFactor::Create(curr_point, center);
						problem.AddResidualBlock(cost_function, loss_function, parameters, parameters + 4);
					}
					*/
				}

				ceres::Solver::Options options;
				options.linear_solver_type = ceres::DENSE_QR;
				options.max_num_iterations = 4;
				options.minimizer_progress_to_stdout = false;
				options.check_gradients = false;
				options.gradient_check_relative_precision = 1e-4;
				ceres::Solver::Summary summary;
				ceres::Solve(options, &problem, &summary);

				//printf("time %f \n", timeLaserOdometry);
				//printf("corner factor num %d surf factor num %d\n", corner_num, surf_num);
				//printf("result q %f %f %f %f result t %f %f %f\n", parameters[3], parameters[0], parameters[1], parameters[2],
				//	   parameters[4], parameters[5], parameters[6]);
			}
		}
		else
		{
		}
		transformUpdate();

		for (int i = 0; i < laserCloudCornerStackNum; i++)
		{
			pointAssociateToMap(&laserCloudCornerStack->points[i], &pointSel);

			int cubeI = int((pointSel.x + 25.0) / 50.0) + laserCloudCenWidth;
			int cubeJ = int((pointSel.y + 25.0) / 50.0) + laserCloudCenHeight;
			int cubeK = int((pointSel.z + 25.0) / 50.0) + laserCloudCenDepth;

			if (pointSel.x + 25.0 < 0)
				cubeI--;
			if (pointSel.y + 25.0 < 0)
				cubeJ--;
			if (pointSel.z + 25.0 < 0)
				cubeK--;

			if (cubeI >= 0 && cubeI < laserCloudWidth &&
				cubeJ >= 0 && cubeJ < laserCloudHeight &&
				cubeK >= 0 && cubeK < laserCloudDepth)
			{
				int cubeInd = cubeI + laserCloudWidth * cubeJ + laserCloudWidth * laserCloudHeight * cubeK;
				laserCloudCornerArray[cubeInd]->push_back(pointSel);
			}
		}

		for (int i = 0; i < laserCloudSurfStackNum; i++)
		{
			pointAssociateToMap(&laserCloudSurfStack->points[i], &pointSel);

			int cubeI = int((pointSel.x + 25.0) / 50.0) + laserCloudCenWidth;
			int cubeJ = int((pointSel.y + 25.0) / 50.0) + laserCloudCenHeight;
			int cubeK = int((pointSel.z + 25.0) / 50.0) + laserCloudCenDepth;

			if (pointSel.x + 25.0 < 0)
				cubeI--;
			if (pointSel.y + 25.0 < 0)
				cubeJ--;
			if (pointSel.z + 25.0 < 0)
				cubeK--;

			if (cubeI >= 0 && cubeI < laserCloudWidth &&
				cubeJ >= 0 && cubeJ < laserCloudHeight &&
				cubeK >= 0 && cubeK < laserCloudDepth)
			{
				int cubeInd = cubeI + laserCloudWidth * cubeJ + laserCloudWidth * laserCloudHeight * cubeK;
				laserCloudSurfArray[cubeInd]->push_back(pointSel);
			}
		}

		
		for (int i = 0; i < laserCloudValidNum; i++)
		{
			int ind = laserCloudValidInd[i];

			pcl::PointCloud<pcl::PointXYZI>::Ptr tmpCorner(new pcl::PointCloud<pcl::PointXYZI>());
			downSizeFilterCorner.setInputCloud(laserCloudCornerArray[ind]);
			downSizeFilterCorner.filter(*tmpCorner);
			laserCloudCornerArray[ind] = tmpCorner;

			pcl::PointCloud<pcl::PointXYZI>::Ptr tmpSurf(new pcl::PointCloud<pcl::PointXYZI>());
			downSizeFilterSurf.setInputCloud(laserCloudSurfArray[ind]);
			downSizeFilterSurf.filter(*tmpSurf);
			laserCloudSurfArray[ind] = tmpSurf;
		}
		
		//publish surround map for every 5 frame
		if (frameCount % 5 == 0)
		{
			laserCloudSurround->clear();
			for (int i = 0; i < laserCloudSurroundNum; i++)
			{
				int ind = laserCloudSurroundInd[i];
				*laserCloudSurround += *laserCloudCornerArray[ind];
				*laserCloudSurround += *laserCloudSurfArray[ind];
			}

			// sensor_msgs::PointCloud2 laserCloudSurround3;
			// pcl::toROSMsg(*laserCloudSurround, laserCloudSurround3);
			// laserCloudSurround3.header.stamp = ros::Time().fromSec(timeLaserOdometry);
			// laserCloudSurround3.header.frame_id = "/camera_init";
			// pubLaserCloudSurround.publish(laserCloudSurround3);
		}

		if (frameCount % 20 == 0)
		{
			pcl::PointCloud<pcl::PointXYZI> laserCloudMap;
			for (int i = 0; i < 4851; i++)
			{
				laserCloudMap += *laserCloudCornerArray[i];
				laserCloudMap += *laserCloudSurfArray[i];
			}
			// sensor_msgs::PointCloud2 laserCloudMsg;
			// pcl::toROSMsg(laserCloudMap, laserCloudMsg);
			// laserCloudMsg.header.stamp = ros::Time().fromSec(timeLaserOdometry);
			// laserCloudMsg.header.frame_id = "/camera_init";
			// pubLaserCloudMap.publish(laserCloudMsg);
		}

		int laserCloudFullResNum = laserCloudFullRes->points.size();
		for (int i = 0; i < laserCloudFullResNum; i++)
		{
			pointAssociateToMap(&laserCloudFullRes->points[i], &laserCloudFullRes->points[i]);
		}

		// sensor_msgs::PointCloud2 laserCloudFullRes3;
		// pcl::toROSMsg(*laserCloudFullRes, laserCloudFullRes3);
		// laserCloudFullRes3.header.stamp = ros::Time().fromSec(timeLaserOdometry);
		// laserCloudFullRes3.header.frame_id = "/camera_init";
		// pubLaserCloudFullRes.publish(laserCloudFullRes3);


		mloam::OdometryData odomAftMapped;
		odomAftMapped.pose.pose.orientation = q_w_curr;
		odomAftMapped.pose.pose.position = t_w_curr;

		// geometry_msgs::PoseStamped laserAfterMappedPose;
		// laserAfterMappedPose.header = odomAftMapped.header;
		// laserAfterMappedPose.pose = odomAftMapped.pose.pose;
		// laserAfterMappedPath.header.stamp = odomAftMapped.header.stamp;
		// laserAfterMappedPath.header.frame_id = "/camera_init";
		// laserAfterMappedPath.poses.push_back(laserAfterMappedPose);
		// pubLaserAfterMappedPath.publish(laserAfterMappedPath);

		// static tf::TransformBroadcaster br;
		// tf::Transform transform;
		// tf::Quaternion q;
		// transform.setOrigin(tf::Vector3(t_w_curr(0),
		// 								t_w_curr(1),
		// 								t_w_curr(2)));
		// q.setW(q_w_curr.w());
		// q.setX(q_w_curr.x());
		// q.setY(q_w_curr.y());
		// q.setZ(q_w_curr.z());
		// transform.setRotation(q);
		// br.sendTransform(tf::StampedTransform(transform, odomAftMapped.header.stamp, "/camera_init", "/aft_mapped"));

		frameCount++;
	// }
	return;
}