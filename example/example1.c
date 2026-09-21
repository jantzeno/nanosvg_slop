//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include <stdio.h>
#include <string.h>
#include <float.h>
#include <GLFW/glfw3.h>

#include "nanosvg.h"

NSVGimage* g_image = NULL;

static unsigned char bgColor[4] = {205,202,200,255};
static unsigned char lineColor[4] = {0,160,192,255};

static double distPtSeg(double x, double y, double px, double py, double qx, double qy)
{
	double pqx, pqy, dx, dy, d, t;
	pqx = qx-px;
	pqy = qy-py;
	dx = x-px;
	dy = y-py;
	d = pqx*pqx + pqy*pqy;
	t = pqx*dx + pqy*dy;
	if (d > 0) t /= d;
	if (t < 0) t = 0;
	else if (t > 1) t = 1;
	dx = px + t*pqx - x;
	dy = py + t*pqy - y;
	return dx*dx + dy*dy;
}

static void cubicBez(double x1, double y1, double x2, double y2,
					 double x3, double y3, double x4, double y4,
					 double tol, int level)
{
	double x12,y12,x23,y23,x34,y34,x123,y123,x234,y234,x1234,y1234;
	double d;
	
	if (level > 12) return;

	x12 = (x1+x2)*0.5;
	y12 = (y1+y2)*0.5;
	x23 = (x2+x3)*0.5;
	y23 = (y2+y3)*0.5;
	x34 = (x3+x4)*0.5;
	y34 = (y3+y4)*0.5;
	x123 = (x12+x23)*0.5;
	y123 = (y12+y23)*0.5;
	x234 = (x23+x34)*0.5;
	y234 = (y23+y34)*0.5;
	x1234 = (x123+x234)*0.5;
	y1234 = (y123+y234)*0.5;

	d = distPtSeg(x1234, y1234, x1,y1, x4,y4);
	if (d > tol*tol) {
		cubicBez(x1,y1, x12,y12, x123,y123, x1234,y1234, tol, level+1); 
		cubicBez(x1234,y1234, x234,y234, x34,y34, x4,y4, tol, level+1); 
	} else {
		glVertex2d(x4, y4);
	}
}

void drawPath(double* pts, int npts, char closed, double tol)
{
	int i;
	glBegin(GL_LINE_STRIP);
	glColor4ubv(lineColor);
	glVertex2d(pts[0], pts[1]);
	for (i = 0; i < npts-1; i += 3) {
		double* p = &pts[i*2];
		cubicBez(p[0],p[1], p[2],p[3], p[4],p[5], p[6],p[7], tol, 0);
	}
	if (closed) {
		glVertex2d(pts[0], pts[1]);
	}
	glEnd();
}

void drawControlPts(double* pts, int npts)
{
	int i;

	// Control lines
	glColor4ubv(lineColor);
	glBegin(GL_LINES);
	for (i = 0; i < npts-1; i += 3) {
		double* p = &pts[i*2];
		glVertex2d(p[0],p[1]);
		glVertex2d(p[2],p[3]);
		glVertex2d(p[4],p[5]);
		glVertex2d(p[6],p[7]);
	}
	glEnd();

	// Points
	glPointSize(6.0);
	glColor4ubv(lineColor);

	glBegin(GL_POINTS);
	glVertex2d(pts[0],pts[1]);
	for (i = 0; i < npts-1; i += 3) {
		double* p = &pts[i*2];
		glVertex2d(p[6],p[7]);
	}
	glEnd();

	// Points
	glPointSize(3.0);

	glBegin(GL_POINTS);
	glColor4ubv(bgColor);
	glVertex2d(pts[0],pts[1]);
	for (i = 0; i < npts-1; i += 3) {
		double* p = &pts[i*2];
		glColor4ubv(lineColor);
		glVertex2d(p[2],p[3]);
		glVertex2d(p[4],p[5]);
		glColor4ubv(bgColor);
		glVertex2d(p[6],p[7]);
	}
	glEnd();
}

void drawframe(GLFWwindow* window)
{
	int width = 0, height = 0;
	double view[4], cx, cy, hw, hh, aspect, px;
	NSVGshape* shape;
	NSVGpath* path;

	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	glfwGetFramebufferSize(window, &width, &height);

	glViewport(0, 0, width, height);
	glClearColor(220.0/255.0, 220.0/255.0, 220.0/255.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_TEXTURE_2D);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	// Fit view to bounds
	cx = g_image->width*0.5;
	cy = g_image->height*0.5;
	hw = g_image->width*0.5;
	hh = g_image->height*0.5;

	if (width/hw < height/hh) {
		aspect = (double)height / (double)width;
		view[0] = cx - hw * 1.2;
		view[2] = cx + hw * 1.2;
		view[1] = cy - hw * 1.2 * aspect;
		view[3] = cy + hw * 1.2 * aspect;
	} else {
		aspect = (double)width / (double)height;
		view[0] = cx - hh * 1.2 * aspect;
		view[2] = cx + hh * 1.2 * aspect;
		view[1] = cy - hh * 1.2;
		view[3] = cy + hh * 1.2;
	}
	// Size of one pixel.
	px = (view[2] - view[1]) / (double)width;

	glOrtho(view[0], view[2], view[3], view[1], -1, 1);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glColor4ub(255,255,255,255);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

	// Draw bounds
	glColor4ub(0,0,0,64);
	glBegin(GL_LINE_LOOP);
	glVertex2d(0, 0);
	glVertex2d(g_image->width, 0);
	glVertex2d(g_image->width, g_image->height);
	glVertex2d(0, g_image->height);
	glEnd();

	for (shape = g_image->shapes; shape != NULL; shape = shape->next) {
		for (path = shape->paths; path != NULL; path = path->next) {
			drawPath(path->pts, path->npts, path->closed, px * 1.5);
			drawControlPts(path->pts, path->npts);
		}
	}

	glfwSwapBuffers(window);
}

void resizecb(GLFWwindow* window, int width, int height)
{
	// Update and render
	(void)width;
	(void)height;
	drawframe(window);
}

int main()
{
	GLFWwindow* window;
	const GLFWvidmode* mode;

	if (!glfwInit())
		return -1;

	mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    window = glfwCreateWindow(mode->width - 40, mode->height - 80, "Nano SVG", NULL, NULL);
	if (!window)
	{
		printf("Could not open window\n");
		glfwTerminate();
		return -1;
	}

	glfwSetFramebufferSizeCallback(window, resizecb);
	glfwMakeContextCurrent(window);
	glEnable(GL_POINT_SMOOTH);
	glEnable(GL_LINE_SMOOTH);


	g_image = nsvgParseFromFile("../example/nano.svg", "px", 96.0);
	if (g_image == NULL) {
		printf("Could not open SVG image.\n");
		glfwTerminate();
		return -1;
	}

	while (!glfwWindowShouldClose(window))
	{
		drawframe(window);
		glfwPollEvents();
	}

	nsvgDelete(g_image);

	glfwTerminate();
	return 0;
}
